#include "audio_mixer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "audio_stream.h"

namespace gfx::audio {

using Clock = std::chrono::steady_clock;

namespace {
constexpr int kMaxStep = 8;                                   // max resample ratio (pitch x rate)
constexpr int kSlotBufFrames = kChunkFrames * kMaxStep + 32;  // source frames held per real voice
constexpr int kHrtfDelay = 128;                               // power of two, > max ITD at 96 kHz

inline uint64_t nsSince(Clock::time_point t0) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-6f ? v * (1.0f / l) : Vec3{0, 0, -1};
}
}  // namespace

const char* toString(Stage s) {
    static const char* const n[] = {"Voice select / virtualize", "Resample", "Filter (occlusion, air)", "Spatialize (pan / HRTF)",
                                    "Bus mix", "Reverb", "Bass management (LFE)", "Dynamics (compressor)",
                                    "Master (volume, mono, limiter, meters)", "Output format + dither"};
    return n[int(s)];
}

const char* toString(MemCategory c) {
    static const char* const n[] = {"Voice pool", "Mix buffers", "Reverb delay lines", "HRTF delay lines"};
    return n[int(c)];
}

Mixer::Mixer() { dsp::initSincTable(); }

void Mixer::configure(uint32_t sampleRate, int channels, const AudioSettings& s) {
    sampleRate_ = sampleRate;
    layout_ = dsp::makeLayout(channels);
    settings_ = s;
    const int ch = layout_.channels;
    hrtfAllocated_ = (s.spatial == SpatialMode::HRTF) && ch == 2;  // binaural only makes sense on 2 channels

    const int nSlots = std::clamp(s.maxVoices, 1, kMaxRealVoices);
    slots_.clear();
    slots_.resize(size_t(nSlots));
    freeSlots_.clear();
    for (int i = nSlots - 1; i >= 0; --i) {
        RealSlot& sl = slots_[size_t(i)];
        for (int c = 0; c < 2; ++c) {
            sl.buf[c].assign(kSlotBufFrames, 0.0f);
            sl.tmp[c].assign(kChunkFrames, 0.0f);
        }
        if (hrtfAllocated_) sl.delay.assign(kHrtfDelay, 0.0f);
        freeSlots_.push_back(i);
    }
    for (auto& r : rt_) r.slot = -1;  // playing voices survive a restart and get re-promoted

    busBuf_.assign(size_t(kBusCount) * ch * kChunkFrames, 0.0f);
    sendBuf_.assign(kChunkFrames, 0.0f);
    wetL_.assign(kChunkFrames, 0.0f);
    wetR_.assign(kChunkFrames, 0.0f);
    master_.assign(size_t(ch) * kChunkFrames, 0.0f);
    interleaved_.assign(size_t(ch) * kChunkFrames, 0.0f);
    fetchScratch_.assign(size_t(kSlotBufFrames) * 2, 0.0f);
    order_.clear();
    order_.reserve(kMaxVoiceRecords);

    reverb_.configure(s.reverb, float(sampleRate));
    reverb_.setParams(s.roomSize, s.damping);
    lfe1_ = {}; lfe2_ = {};
    lfe1_.setLowpass(s.lfeCrossoverHz, 0.7071f, float(sampleRate));
    lfe2_.setLowpass(s.lfeCrossoverHz, 0.7071f, float(sampleRate));
    comp_ = {}; limiter_ = {};
    dsp::configureDynamics(s.dynamicRange, float(sampleRate), comp_, limiter_);
    for (int b = 0; b < kBusCount; ++b) busGain_[size_t(b)] = s.busVolume[size_t(b)];
    masterGain_ = s.muteAll ? 0.0f : s.masterVolume;
    updateMemory();
}

void Mixer::updateMemory() {
    size_t pool = sizeof(rt_) + sizeof(descs_) + slots_.capacity() * sizeof(RealSlot), hrtf = 0;
    for (auto& sl : slots_) {
        for (int c = 0; c < 2; ++c) pool += (sl.buf[c].capacity() + sl.tmp[c].capacity()) * sizeof(float);
        hrtf += sl.delay.capacity() * sizeof(float);
    }
    size_t mix = (busBuf_.capacity() + sendBuf_.capacity() + wetL_.capacity() + wetR_.capacity() + master_.capacity() +
                  interleaved_.capacity() + fetchScratch_.capacity()) * sizeof(float);
    counters_.memBytes[size_t(MemCategory::VoicePool)] = pool;
    counters_.memBytes[size_t(MemCategory::MixBuffers)] = mix;
    counters_.memBytes[size_t(MemCategory::Reverb)] = reverb_.memoryBytes();
    counters_.memBytes[size_t(MemCategory::Hrtf)] = hrtf;
}

void Mixer::sync(const AudioSettings& s, const Listener& l, bool focused, std::array<VoiceDesc, kMaxVoiceRecords>& descs,
                 std::array<VoiceFeedback, kMaxVoiceRecords>& feedback, MixerCounters& counters) {
    const bool dynChanged = s.dynamicRange != settings_.dynamicRange;
    const bool lfeChanged = s.lfeCrossoverHz != settings_.lfeCrossoverHz;
    const AudioSettings keep = settings_;
    settings_ = s;
    // Fields that size allocations stay as configured until the next configure().
    settings_.maxVoices = keep.maxVoices;
    settings_.reverb = keep.reverb;
    if (!hrtfAllocated_ && settings_.spatial == SpatialMode::HRTF) settings_.spatial = SpatialMode::SpeakerPanning;

    reverb_.setParams(settings_.roomSize, settings_.damping);
    if (dynChanged) dsp::configureDynamics(settings_.dynamicRange, float(sampleRate_), comp_, limiter_);
    if (lfeChanged) {
        lfe1_.setLowpass(settings_.lfeCrossoverHz, 0.7071f, float(sampleRate_));
        lfe2_.setLowpass(settings_.lfeCrossoverHz, 0.7071f, float(sampleRate_));
    }
    listener_ = l;
    focused_ = focused;

    for (int i = 0; i < kMaxVoiceRecords; ++i) {
        const VoiceDesc& d = descs[size_t(i)];
        VoiceRuntime& r = rt_[size_t(i)];
        if (d.generation != r.generation) {
            if (r.slot >= 0) releaseSlot(i);
            r = VoiceRuntime{};
            r.generation = d.generation;
            r.playing = d.active;
        } else if (!d.active && r.playing && !r.stopping) {
            r.stopping = true;
        }
        descs_[size_t(i)] = d;
        feedback[size_t(i)] = {r.generation, r.finished, r.slot >= 0, r.audibility};
    }
    counters = counters_;
}

float Mixer::attenuation(const VoiceDesc& d, float dist) const {
    const float ref = std::max(0.01f, d.refDistance);
    const float mx = std::max(ref + 0.01f, d.maxDistance);
    const float dd = std::clamp(dist, ref, mx);
    switch (settings_.attenuation) {
        case AttenuationModel::None: return 1.0f;
        case AttenuationModel::Linear: return std::clamp(1.0f - settings_.rolloff * (dd - ref) / (mx - ref), 0.0f, 1.0f);
        case AttenuationModel::Exponential: return std::pow(dd / ref, -settings_.rolloff);
        case AttenuationModel::Inverse:
        default: return ref / (ref + settings_.rolloff * (dd - ref));
    }
}

void Mixer::releaseSlot(int index) {
    VoiceRuntime& r = rt_[size_t(index)];
    if (r.slot < 0) return;
    slots_[size_t(r.slot)].owner = -1;
    slots_[size_t(r.slot)].demoting = false;
    freeSlots_.push_back(r.slot);
    r.slot = -1;
}

void Mixer::selectVoices() {
    order_.clear();
    for (int i = 0; i < kMaxVoiceRecords; ++i) {
        VoiceRuntime& r = rt_[size_t(i)];
        if (!r.playing) continue;
        const VoiceDesc& d = descs_[size_t(i)];
        float a = d.volume * settings_.busVolume[size_t(d.bus)];
        if (d.is3D) {
            float dist = length(d.position - listener_.position);
            a *= attenuation(d, dist) * (1.0f - 0.75f * r.occlusion);
        }
        float score = a * (0.5f + float(d.priority) / 255.0f);
        if (d.stream >= 0) score += 10.0f;   // never virtualize a stream: its ring must keep draining
        if (r.slot >= 0) score *= 1.25f;     // hysteresis so voices near the cut-off don't flap
        r.audibility = score;
        order_.push_back(i);
    }
    const size_t nReal = std::min(order_.size(), slots_.size());
    if (nReal < order_.size())
        std::nth_element(order_.begin(), order_.begin() + long(nReal), order_.end(),
                         [&](int a, int b) { return rt_[size_t(a)].audibility > rt_[size_t(b)].audibility; });
    int real = 0, virt = 0;
    for (size_t k = 0; k < order_.size(); ++k) {
        int i = order_[k];
        VoiceRuntime& r = rt_[size_t(i)];
        if (k < nReal) {
            if (r.slot >= 0) { slots_[size_t(r.slot)].demoting = false; ++real; continue; }
            if (freeSlots_.empty()) { ++virt; continue; }  // a demoting voice still holds it; next chunk
            r.slot = freeSlots_.back();
            freeSlots_.pop_back();
            RealSlot& sl = slots_[size_t(r.slot)];
            sl.owner = i;
            sl.count = 0;
            sl.base = int64_t(std::floor(r.srcPos)) - dsp::kResampleTapsBefore;
            sl.fetchCursor = sl.base;
            std::fill(std::begin(sl.prevGains), std::end(sl.prevGains), 0.0f);  // fade in from silence
            sl.prevSend = 0.0f;
            sl.demoting = false;
            sl.lp[0].z = sl.lp[1].z = 0.0f;
            if (!sl.delay.empty()) std::fill(sl.delay.begin(), sl.delay.end(), 0.0f);
            sl.itdL = sl.itdR = 0.0f;
            sl.shadowL = {}; sl.shadowR = {}; sl.rearL = {}; sl.rearR = {};
            ++real;
        } else {
            if (r.slot >= 0) slots_[size_t(r.slot)].demoting = true;  // fade out this chunk, then go virtual
            ++virt;
        }
    }
    counters_.realVoices = real;
    counters_.virtualVoices = virt;
}

int Mixer::fetch(RealSlot& sl, const VoiceDesc& d, int64_t needUpTo) {
    int fetched = 0;
    while (sl.fetchCursor <= needUpTo && sl.count < kSlotBufFrames) {
        int n = int(std::min<int64_t>(needUpTo - sl.fetchCursor + 1, kSlotBufFrames - sl.count));
        if (d.stream >= 0) {
            StreamSource* st = (streams_ && d.stream < int(streams_->size())) ? (*streams_)[size_t(d.stream)] : nullptr;
            const int sch = st ? st->channels() : 1;
            if (sl.fetchCursor < 0) {  // leading history before the stream starts
                n = std::min<int>(n, int(-sl.fetchCursor));
                for (int c = 0; c < 2; ++c) std::fill_n(&sl.buf[c][size_t(sl.count)], n, 0.0f);
            } else if (st) {
                st->pumpSync(n);
                st->read(fetchScratch_.data(), n);
                for (int k = 0; k < n; ++k)
                    for (int c = 0; c < std::min(sch, 2); ++c) sl.buf[c][size_t(sl.count + k)] = fetchScratch_[size_t(k * sch + c)];
            } else {
                for (int c = 0; c < 2; ++c) std::fill_n(&sl.buf[c][size_t(sl.count)], n, 0.0f);
            }
        } else {
            const SoundAsset& a = (*assets_)[size_t(d.sound)];
            const int64_t len = int64_t(a.frames());
            const int sch = a.channels;
            for (int k = 0; k < n; ++k) {
                int64_t idx = sl.fetchCursor + k;
                bool valid = true;
                if (d.loop) idx = ((idx % len) + len) % len;
                else valid = idx >= 0 && idx < len;
                for (int c = 0; c < std::min(sch, 2); ++c)
                    sl.buf[c][size_t(sl.count + k)] = valid ? a.data[size_t(idx * sch + c)] : 0.0f;
            }
        }
        sl.count += n;
        sl.fetchCursor += n;
        fetched += n;
    }
    return fetched;
}

void Mixer::renderVoice(int i, int n) {
    VoiceRuntime& r = rt_[size_t(i)];
    const VoiceDesc& d = descs_[size_t(i)];
    RealSlot& sl = slots_[size_t(r.slot)];
    const int ch = layout_.channels;

    uint32_t srcRate = 0;
    int srcCh = 1;
    int64_t srcLen = -1;
    if (d.stream >= 0 && streams_ && d.stream < int(streams_->size()) && (*streams_)[size_t(d.stream)]) {
        srcRate = (*streams_)[size_t(d.stream)]->sampleRate();
        srcCh = std::min((*streams_)[size_t(d.stream)]->channels(), 2);
    } else if (d.sound >= 0 && assets_ && d.sound < int(assets_->size())) {
        const SoundAsset& a = (*assets_)[size_t(d.sound)];
        srcRate = a.sampleRate;
        srcCh = std::min(a.channels, 2);
        srcLen = int64_t(a.frames());
    }
    if (srcRate == 0) { r.finished = true; r.playing = false; releaseSlot(i); return; }

    // ---- per-chunk parameters ----
    auto t0 = Clock::now();
    float target[kMaxChannels]{};
    float pitch = d.pitch, cutoff = 20000.0f, send = 0.0f, gain = d.volume;
    bool hrtf = false;
    float lx = 0, lz = 1, dist = 0;
    if (d.is3D) {
        const Vec3 rel = d.position - listener_.position;
        dist = length(rel);
        const Vec3 f = normalize(listener_.forward);
        const Vec3 right = normalize(cross(f, listener_.up));
        if (dist > 1e-3f) { lx = dot(rel, right) / dist; lz = dot(rel, f) / dist; }
        const float az = std::atan2(lx, lz) * 180.0f / dsp::kPi;
        const float att = attenuation(d, dist);
        const float occTarget = settings_.occlusion == OcclusionMode::Off ? 0.0f : d.occlusion;
        r.occlusion += (occTarget - r.occlusion) * 0.2f;
        gain *= att * dsp::lerpf(1.0f, 0.3f, r.occlusion);
        cutoff = 20000.0f * std::pow(600.0f / 20000.0f, r.occlusion);
        if (settings_.airAbsorption) cutoff = std::min(cutoff, 20000.0f * std::exp(-dist / 100.0f));
        if (settings_.doppler) {
            // OpenAL 1.1 Doppler: SL points from source to listener.
            const Vec3 sl3 = listener_.position - d.position;
            const float lenSL = std::max(length(sl3), 1e-3f);
            const float c = dsp::kSpeedOfSound, df = settings_.dopplerFactor;
            const float vls = std::min(dot(sl3, listener_.velocity) / lenSL, c / std::max(df, 1e-3f) * 0.99f);
            const float vss = std::min(dot(sl3, d.velocity) / lenSL, c / std::max(df, 1e-3f) * 0.99f);
            const float dp = std::clamp((c - df * vls) / (c - df * vss), 0.25f, 4.0f);
            r.dopplerPitch += (dp - r.dopplerPitch) * 0.3f;
        } else {
            r.dopplerPitch = 1.0f;
        }
        pitch *= r.dopplerPitch;
        if (settings_.spatial == SpatialMode::HRTF && hrtfAllocated_) {
            hrtf = true;
            target[0] = gain;
        } else if (settings_.spatial == SpatialMode::SpeakerPanning) {
            const float spread = std::clamp(1.0f - dist / std::max(d.refDistance, 0.01f), 0.0f, 1.0f) * 0.8f;
            dsp::vbapGains(layout_, az, spread, target);
            for (int c = 0; c < ch; ++c) target[c] *= gain;
        } else {
            dsp::stereoPanGains(layout_, az, target);
            for (int c = 0; c < ch; ++c) target[c] *= gain;
        }
        send = d.reverbSend * d.volume * std::sqrt(att) * (1.0f + 0.5f * r.occlusion);
    } else {
        if (srcCh == 2) {
            target[0] = gain;
            target[1] = gain;
        } else {
            dsp::stereoPanGains(layout_, 0.0f, target);
            for (int c = 0; c < ch; ++c) target[c] *= gain;
        }
        send = d.reverbSend * d.volume * 0.3f;
    }
    const bool fadingOut = r.stopping || sl.demoting;
    if (fadingOut) {
        std::fill(std::begin(target), std::end(target), 0.0f);
        send = 0.0f;
    }
    const double step = std::min(double(pitch) * srcRate / sampleRate_, double(kMaxStep) - 0.5);
    counters_.stageNs[size_t(Stage::Spatialize)] += nsSince(t0);

    // ---- fetch + resample ----
    t0 = Clock::now();
    const int64_t lastNeeded = int64_t(std::floor(r.srcPos + (n - 1) * step)) + dsp::kResampleTapsAfter;
    fetch(sl, d, lastNeeded);
    const double posRel = r.srcPos - double(sl.base);
    for (int c = 0; c < srcCh; ++c) dsp::resample(settings_.resampler, sl.buf[c].data(), posRel, step, sl.tmp[c].data(), n);
    r.srcPos += n * step;
    const int64_t drop = (int64_t(std::floor(r.srcPos)) - dsp::kResampleTapsBefore) - sl.base;
    if (drop > 0) {
        const int keep = std::max(0, sl.count - int(drop));
        for (int c = 0; c < srcCh; ++c) std::memmove(sl.buf[c].data(), sl.buf[c].data() + drop, sizeof(float) * size_t(keep));
        sl.count = keep;
        sl.base += drop;
        if (sl.fetchCursor < sl.base) sl.fetchCursor = sl.base;
    }
    counters_.stageNs[size_t(Stage::Resample)] += nsSince(t0);

    // ---- filter (occlusion + air absorption low-pass) ----
    t0 = Clock::now();
    if (cutoff < 19000.0f) {
        for (int c = 0; c < srcCh; ++c) {
            sl.lp[c].setCutoff(cutoff, float(sampleRate_));
            float* x = sl.tmp[c].data();
            for (int k = 0; k < n; ++k) x[k] = sl.lp[c].process(x[k]);
        }
    } else {
        for (int c = 0; c < srcCh; ++c) sl.lp[c].z = sl.tmp[c][size_t(n - 1)];  // keep state primed: no click when it re-engages
    }
    counters_.stageNs[size_t(Stage::Filter)] += nsSince(t0);

    // ---- spatialize into the bus ----
    t0 = Clock::now();
    float* bus = &busBuf_[size_t(int(d.bus)) * ch * kChunkFrames];
    const float invN = 1.0f / float(n);
    // mono version of the source for 3D paths and the reverb send
    float* mono = sl.tmp[0].data();
    if (srcCh == 2 && (d.is3D || ch == 1)) {
        for (int k = 0; k < n; ++k) mono[k] = 0.5f * (sl.tmp[0][size_t(k)] + sl.tmp[1][size_t(k)]);
    }
    if (hrtf) {
        // Spherical-head HRTF: per-ear ITD (fractional delay) + head-shadow + rear low-pass.
        const float sinLat = std::min(std::abs(lx), 1.0f);
        const float lat = std::asin(sinLat);
        const float itd = dsp::kHeadRadius / dsp::kSpeedOfSound * (lat + std::sin(lat)) * float(sampleRate_);
        const float itdL1 = lx > 0 ? itd : 0.0f, itdR1 = lx > 0 ? 0.0f : itd;
        auto alpha = [](float cosTheta) {
            const float thetaDeg = std::acos(std::clamp(cosTheta, -1.0f, 1.0f)) * 180.0f / dsp::kPi;
            const float aMin = 0.1f, thetaMin = 150.0f;
            return (1.0f + aMin * 0.5f) + (1.0f - aMin * 0.5f) * std::cos(std::min(thetaDeg, thetaMin) / thetaMin * dsp::kPi);
        };
        sl.shadowL.set(alpha(-lx), float(sampleRate_));
        sl.shadowR.set(alpha(lx), float(sampleRate_));
        const float rear = std::max(0.0f, -lz);
        const float rc = 18000.0f * std::pow(5000.0f / 18000.0f, rear);
        sl.rearL.setCutoff(rc, float(sampleRate_));
        sl.rearR.setCutoff(rc, float(sampleRate_));
        const float g0 = sl.prevGains[0], g1 = target[0];
        const float l0 = sl.itdL, r0 = sl.itdR;
        float* outL = bus;
        float* outR = bus + kChunkFrames;
        const int mask = kHrtfDelay - 1;
        for (int k = 0; k < n; ++k) {
            const float t = float(k + 1) * invN;
            sl.delay[size_t(sl.delayW)] = mono[k];
            auto tap = [&](float dl) {
                const float rp = float(sl.delayW) - dl;
                const int ip = int(std::floor(rp));
                const float fr = rp - float(ip);
                return sl.delay[size_t(ip & mask)] * (1.0f - fr) + sl.delay[size_t((ip + 1) & mask)] * fr;
            };
            const float g = dsp::lerpf(g0, g1, t);
            outL[k] += sl.rearL.process(sl.shadowL.process(tap(dsp::lerpf(l0, itdL1, t)))) * g;
            outR[k] += sl.rearR.process(sl.shadowR.process(tap(dsp::lerpf(r0, itdR1, t)))) * g;
            sl.delayW = (sl.delayW + 1) & mask;
        }
        sl.itdL = itdL1;
        sl.itdR = itdR1;
        sl.prevGains[0] = g1;
    } else if (!d.is3D && srcCh == 2 && ch >= 2) {
        for (int c = 0; c < 2; ++c) {
            const float g0 = sl.prevGains[c], g1 = target[c];
            float* o = bus + size_t(c) * kChunkFrames;
            const float* x = sl.tmp[size_t(c)].data();
            for (int k = 0; k < n; ++k) o[k] += x[k] * dsp::lerpf(g0, g1, float(k + 1) * invN);
            sl.prevGains[c] = g1;
        }
    } else {
        for (int c = 0; c < ch; ++c) {
            const float g0 = sl.prevGains[c], g1 = target[c];
            if (g0 == 0.0f && g1 == 0.0f) continue;
            float* o = bus + size_t(c) * kChunkFrames;
            for (int k = 0; k < n; ++k) o[k] += mono[k] * dsp::lerpf(g0, g1, float(k + 1) * invN);
            sl.prevGains[c] = g1;
        }
    }
    if (settings_.reverb != ReverbQuality::Off && (send > 0.0f || sl.prevSend > 0.0f)) {
        for (int k = 0; k < n; ++k) sendBuf_[size_t(k)] += mono[k] * dsp::lerpf(sl.prevSend, send, float(k + 1) * invN);
    }
    sl.prevSend = send;
    counters_.stageNs[size_t(Stage::Spatialize)] += nsSince(t0);

    // ---- lifetime ----
    if (srcLen >= 0 && !d.loop && r.srcPos >= double(srcLen)) {
        r.finished = true;
        r.playing = false;
        releaseSlot(i);
    } else if (r.stopping) {
        r.finished = true;
        r.playing = false;
        releaseSlot(i);
    } else if (sl.demoting) {
        releaseSlot(i);
    }
}

void Mixer::render(float* out, int frames) {
    int done = 0;
    while (done < frames) {
        const int n = std::min(kChunkFrames, frames - done);
        renderChunk(out + size_t(done) * layout_.channels, n);
        done += n;
    }
}

void Mixer::renderChunk(float* out, int n) {
    const int ch = layout_.channels;
    const float sr = float(sampleRate_);

    auto t0 = Clock::now();
    selectVoices();
    std::fill(busBuf_.begin(), busBuf_.end(), 0.0f);
    std::fill_n(sendBuf_.begin(), n, 0.0f);
    // advance virtual voices without mixing them (that is the whole saving)
    for (int i = 0; i < kMaxVoiceRecords; ++i) {
        VoiceRuntime& r = rt_[size_t(i)];
        if (!r.playing || r.slot >= 0) continue;
        const VoiceDesc& d = descs_[size_t(i)];
        if (r.stopping) { r.playing = false; r.finished = true; continue; }
        if (d.sound >= 0 && assets_ && d.sound < int(assets_->size())) {
            const SoundAsset& a = (*assets_)[size_t(d.sound)];
            r.srcPos += double(n) * d.pitch * a.sampleRate / sampleRate_;
            if (!d.loop && r.srcPos >= double(a.frames())) { r.playing = false; r.finished = true; }
        }
    }
    counters_.stageNs[size_t(Stage::VoiceSelect)] += nsSince(t0);

    for (int i = 0; i < kMaxVoiceRecords; ++i)
        if (rt_[size_t(i)].slot >= 0) renderVoice(i, n);

    // ---- buses -> master ----
    t0 = Clock::now();
    std::fill(master_.begin(), master_.end(), 0.0f);
    const float invN = 1.0f / float(n);
    for (int b = 0; b < kBusCount; ++b) {
        const float g0 = busGain_[size_t(b)], g1 = settings_.busVolume[size_t(b)];
        for (int c = 0; c < ch; ++c) {
            const float* src = &busBuf_[(size_t(b) * ch + c) * kChunkFrames];
            float* dst = &master_[size_t(c) * kChunkFrames];
            for (int k = 0; k < n; ++k) dst[k] += src[k] * dsp::lerpf(g0, g1, float(k + 1) * invN);
        }
        busGain_[size_t(b)] = g1;
    }
    counters_.stageNs[size_t(Stage::BusMix)] += nsSince(t0);

    // ---- reverb return ----
    t0 = Clock::now();
    if (reverb_.quality() != ReverbQuality::Off) {
        std::fill_n(wetL_.begin(), n, 0.0f);
        std::fill_n(wetR_.begin(), n, 0.0f);
        reverb_.process(sendBuf_.data(), wetL_.data(), wetR_.data(), n);
        const float wet = settings_.reverbWet;
        if (ch == 1) {
            for (int k = 0; k < n; ++k) master_[size_t(k)] += wet * 0.5f * (wetL_[size_t(k)] + wetR_[size_t(k)]);
        } else {
            for (int c = 0; c < ch; ++c) {
                if (c == layout_.lfeIndex) continue;
                float g = (c < 2) ? wet : (layout_.isRear[c] ? wet * 0.7f : 0.0f);
                if (g == 0.0f) continue;
                const float* w = (c == 0 || (c >= 2 && layout_.azimuthDeg[c] < 0)) ? wetL_.data() : wetR_.data();
                float* dst = &master_[size_t(c) * kChunkFrames];
                for (int k = 0; k < n; ++k) dst[k] += g * w[k];
            }
        }
    }
    counters_.stageNs[size_t(Stage::Reverb)] += nsSince(t0);

    // ---- bass management: LFE = 4th-order low-pass of the sum of the mains ----
    t0 = Clock::now();
    if (layout_.lfeIndex >= 0) {
        float* lfe = &master_[size_t(layout_.lfeIndex) * kChunkFrames];
        for (int k = 0; k < n; ++k) {
            float s = 0.0f;
            for (int c = 0; c < ch; ++c)
                if (c != layout_.lfeIndex) s += master_[size_t(c) * kChunkFrames + k];
            lfe[k] = lfe2_.process(lfe1_.process(s * 0.5f));
        }
    }
    counters_.stageNs[size_t(Stage::BassManagement)] += nsSince(t0);

    for (int k = 0; k < n; ++k)
        for (int c = 0; c < ch; ++c) out[size_t(k) * ch + c] = master_[size_t(c) * kChunkFrames + k];

    // ---- dynamic range compression (night mode) ----
    t0 = Clock::now();
    const float compGr = comp_.process(out, n, ch);
    counters_.stageNs[size_t(Stage::Dynamics)] += nsSince(t0);

    // ---- master: volume, background fade, mono, balance, safety limiter, meters ----
    t0 = Clock::now();
    const float mTarget = settings_.muteAll ? 0.0f : settings_.masterVolume;
    float bgTarget = 1.0f;
    if (!focused_) {
        switch (settings_.background) {
            case BackgroundAudio::Duck: bgTarget = settings_.duckVolume; break;
            case BackgroundAudio::Mute:
            case BackgroundAudio::Pause: bgTarget = 0.0f; break;
            default: break;
        }
    }
    const float bgStep = float(n) / (0.25f * sr);  // 250 ms fade
    const float bg1 = bgGain_ < bgTarget ? std::min(bgTarget, bgGain_ + bgStep) : std::max(bgTarget, bgGain_ - bgStep);
    const float m0 = masterGain_ * bgGain_, m1 = mTarget * bg1;
    float balL = 1.0f, balR = 1.0f;
    if (settings_.balance > 0) balL = 1.0f - settings_.balance;
    if (settings_.balance < 0) balR = 1.0f + settings_.balance;
    int mains = 0;
    for (int c = 0; c < ch; ++c) mains += (c != layout_.lfeIndex);
    for (int k = 0; k < n; ++k) {
        float* f = out + size_t(k) * ch;
        const float g = dsp::lerpf(m0, m1, float(k + 1) * invN);
        if (settings_.monoAudio && ch > 1) {
            float m = 0.0f;
            for (int c = 0; c < ch; ++c) if (c != layout_.lfeIndex) m += f[c];
            m /= float(mains);
            for (int c = 0; c < ch; ++c) if (c != layout_.lfeIndex) f[c] = m;
        }
        for (int c = 0; c < ch; ++c) {
            float b = 1.0f;
            if (ch > 1 && c != layout_.lfeIndex) b = layout_.azimuthDeg[c] < 0 ? balL : (layout_.azimuthDeg[c] > 0 ? balR : 1.0f);
            f[c] *= g * b;
        }
    }
    masterGain_ = mTarget;
    bgGain_ = bg1;
    const float limGr = limiter_.process(out, n, ch);
    for (int c = 0; c < ch; ++c) {
        float pk = counters_.peak[size_t(c)] * std::exp(-float(n) / (0.3f * sr));  // 300 ms peak decay
        double sum = 0.0;
        for (int k = 0; k < n; ++k) {
            float& v = out[size_t(k) * ch + c];
            v = std::clamp(v, -1.0f, 1.0f);
            pk = std::max(pk, std::abs(v));
            sum += double(v) * v;
        }
        counters_.peak[size_t(c)] = pk;
        const float rmsNow = float(std::sqrt(sum / n));
        counters_.rms[size_t(c)] = 0.9f * counters_.rms[size_t(c)] + 0.1f * rmsNow;
    }
    counters_.gainReductionDb = std::max(compGr, limGr);
    counters_.backgroundGain = bgGain_;
    counters_.stageNs[size_t(Stage::Master)] += nsSince(t0);
    counters_.framesRendered += uint64_t(n);
}

void Mixer::renderToFormat(void* out, SampleFormatSetting fmt, int frames) {
    const int ch = layout_.channels;
    int done = 0;
    while (done < frames) {
        const int n = std::min(kChunkFrames, frames - done);
        renderChunk(interleaved_.data(), n);
        auto t0 = Clock::now();
        const size_t count = size_t(n) * ch;
        const bool dither = settings_.dither;
        switch (fmt) {
            case SampleFormatSetting::F32:
                std::memcpy(static_cast<float*>(out) + size_t(done) * ch, interleaved_.data(), count * sizeof(float));
                break;
            case SampleFormatSetting::S16: {
                int16_t* o = static_cast<int16_t*>(out) + size_t(done) * ch;
                for (size_t k = 0; k < count; ++k) {
                    float v = interleaved_[k] * 32767.0f + (dither ? dither_.tpdf() : 0.0f);
                    o[k] = int16_t(std::clamp(std::lrint(v), -32768L, 32767L));
                }
                break;
            }
            case SampleFormatSetting::S24:
            default: {
                uint8_t* o = static_cast<uint8_t*>(out) + size_t(done) * ch * 3;
                for (size_t k = 0; k < count; ++k) {
                    float v = interleaved_[k] * 8388607.0f + (dither ? dither_.tpdf() : 0.0f);
                    int32_t s = int32_t(std::clamp(std::lrint(v), -8388608L, 8388607L));
                    o[k * 3 + 0] = uint8_t(s & 0xFF);
                    o[k * 3 + 1] = uint8_t((s >> 8) & 0xFF);
                    o[k * 3 + 2] = uint8_t((s >> 16) & 0xFF);
                }
                break;
            }
        }
        counters_.stageNs[size_t(Stage::Output)] += nsSince(t0);
        done += n;
    }
}

}  // namespace gfx::audio
