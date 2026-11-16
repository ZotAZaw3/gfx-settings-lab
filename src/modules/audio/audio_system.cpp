#include "audio_system.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../../../third_party/miniaudio/miniaudio.h"

namespace gfx::audio {

namespace {
using Clock = std::chrono::steady_clock;

void maCallback(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frames) {
    static_cast<AudioSystem*>(dev->pUserData)->onAudio(out, frames);
}

ma_format toMa(SampleFormatSetting f) {
    switch (f) {
        case SampleFormatSetting::S16: return ma_format_s16;
        case SampleFormatSetting::S24: return ma_format_s24;
        default: return ma_format_f32;
    }
}

const ma_channel* channelMap(int ch) {
    // Must match dsp::makeLayout() order.
    static const ma_channel m1[] = {MA_CHANNEL_MONO};
    static const ma_channel m2[] = {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT};
    static const ma_channel m4[] = {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_BACK_LEFT, MA_CHANNEL_BACK_RIGHT};
    static const ma_channel m6[] = {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE,
                                    MA_CHANNEL_SIDE_LEFT, MA_CHANNEL_SIDE_RIGHT};
    static const ma_channel m8[] = {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE,
                                    MA_CHANNEL_BACK_LEFT, MA_CHANNEL_BACK_RIGHT, MA_CHANNEL_SIDE_LEFT, MA_CHANNEL_SIDE_RIGHT};
    switch (ch) {
        case 1: return m1;
        case 2: return m2;
        case 4: return m4;
        case 6: return m6;
        case 8: return m8;
        default: return nullptr;
    }
}

bool segmentHitsBox(const Vec3& p0, const Vec3& p1, const Vec3& mn, const Vec3& mx) {
    const float o[3] = {p0.x, p0.y, p0.z}, d[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
    float tmin = 0.0f, tmax = 1.0f;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-8f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return false;
            continue;
        }
        float t1 = (lo[a] - o[a]) / d[a], t2 = (hi[a] - o[a]) / d[a];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    return true;
}
}  // namespace

std::vector<std::string> enumerateOutputDevices() {
    std::vector<std::string> names;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < count; ++i) names.emplace_back(infos[i].name);
    ma_context_uninit(&ctx);
    return names;
}

AudioSystem::AudioSystem() {
    assets_.reserve(64);
    streams_.reserve(16);
    streamPtrs_.reserve(16);
}

AudioSystem::~AudioSystem() { shutdown(); }

bool AudioSystem::init(const AudioSettings& s, const std::string& cacheDir) {
    settings_ = s;
    cacheDir_ = cacheDir;
    auto* ctx = new ma_context;
    if (ma_context_init(nullptr, 0, nullptr, ctx) != MA_SUCCESS) {
        ma_backend nullBackend[] = {ma_backend_null};  // headless CI: keep running, silently
        if (ma_context_init(nullBackend, 1, nullptr, ctx) != MA_SUCCESS) {
            delete ctx;
            return false;
        }
    }
    context_ = ctx;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    deviceNames_.clear();
    deviceIds_.clear();
    static_assert(sizeof(ma_device_id) <= 256, "device id storage too small");
    if (ma_context_get_devices(ctx, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            deviceNames_.emplace_back(infos[i].name);
            std::array<unsigned char, 256> id{};
            std::memcpy(id.data(), &infos[i].id, sizeof(ma_device_id));
            deviceIds_.push_back(id);
        }
    }
    mixer_.setAssets(&assets_);
    mixer_.setStreams(&streamPtrs_);
    if (openDevice()) return true;
    // A backend can initialise yet have no usable device (headless servers, no
    // sound card). Fall back to the null backend so the app and stats still run.
    ma_context_uninit(ctx);
    ma_backend nullBackend[] = {ma_backend_null};
    if (ma_context_init(nullBackend, 1, nullptr, ctx) != MA_SUCCESS) {
        delete ctx;
        context_ = nullptr;
        return false;
    }
    settings_.deviceIndex = -1;
    return openDevice();
}

void AudioSystem::shutdown() {
    closeDevice();
    for (auto& st : streams_) st->close();
    streams_.clear();
    streamPtrs_.clear();
    if (context_) {
        ma_context_uninit(static_cast<ma_context*>(context_));
        delete static_cast<ma_context*>(context_);
        context_ = nullptr;
    }
}

bool AudioSystem::openDevice() {
    auto* ctx = static_cast<ma_context*>(context_);
    if (!ctx) return false;
    auto tryInit = [&](ma_format fmt, int ch, uint32_t rate, ma_device*& out) {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        if (settings_.deviceIndex >= 0 && settings_.deviceIndex < int(deviceIds_.size()))
            cfg.playback.pDeviceID = reinterpret_cast<ma_device_id*>(deviceIds_[size_t(settings_.deviceIndex)].data());
        cfg.playback.format = fmt;
        cfg.playback.channels = ma_uint32(ch);
        cfg.playback.pChannelMap = ch ? const_cast<ma_channel*>(channelMap(ch)) : nullptr;
        cfg.sampleRate = rate;
        cfg.periodSizeInFrames = ma_uint32(std::max(32, settings_.bufferFrames));
        cfg.periods = ma_uint32(std::clamp(settings_.periods, 2, 8));
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.noClip = MA_TRUE;  // the mixer's limiter + clamp owns clipping
        cfg.dataCallback = maCallback;
        cfg.pUserData = this;
        auto* dev = new ma_device;
        if (ma_device_init(ctx, &cfg, dev) != MA_SUCCESS) {
            delete dev;
            return false;
        }
        out = dev;
        return true;
    };

    ma_device* dev = nullptr;
    const int wantCh = speakerChannels(settings_.speakers);
    if (!tryInit(toMa(settings_.format), wantCh, sampleRateHz(settings_.sampleRate), dev) &&
        !tryInit(ma_format_f32, 2, 0, dev))
        return false;
    int ch = int(dev->playback.channels);
    if (!channelMap(ch)) {  // e.g. a 3- or 10-channel device: mix stereo and let miniaudio up/downmix
        ma_device_uninit(dev);
        delete dev;
        dev = nullptr;
        if (!tryInit(toMa(settings_.format), 2, sampleRateHz(settings_.sampleRate), dev)) return false;
        ch = 2;
    }
    device_ = dev;
    deviceFormat_ = settings_.format;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        mixer_.configure(dev->sampleRate, ch, settings_);
    }
    char name[256] = {};
    ma_device_get_name(dev, ma_device_type_playback, name, sizeof(name), nullptr);
    stats_.backend = ma_get_backend_name(ctx->backend);
    stats_.device = name;
    stats_.channels = ch;
    stats_.sampleRate = dev->sampleRate;
    stats_.periodFrames = int(dev->playback.internalPeriodSizeInFrames);
    stats_.periods = int(dev->playback.internalPeriods);
    stats_.bufferLatencyMs = 1000.0 * stats_.periodFrames * stats_.periods / std::max<ma_uint32>(dev->playback.internalSampleRate, 1);
    stats_.deviceBufferBytes = size_t(stats_.periodFrames) * stats_.periods * dev->playback.internalChannels *
                               ma_get_bytes_per_sample(dev->playback.internalFormat);
    stats_.format = std::string(toString(settings_.format)) + " (device native: " + ma_get_format_name(dev->playback.internalFormat) +
                    ", " + std::to_string(dev->playback.internalChannels) + " ch, " +
                    std::to_string(dev->playback.internalSampleRate) + " Hz)";
    deviceRunning_ = ma_device_start(dev) == MA_SUCCESS;
    lastCounters_ = MixerCounters{};
    return true;
}

void AudioSystem::closeDevice() {
    if (!device_) return;
    ma_device_uninit(static_cast<ma_device*>(device_));  // stops and joins the audio thread
    delete static_cast<ma_device*>(device_);
    device_ = nullptr;
    deviceRunning_ = false;
}

void AudioSystem::restartStreams() {
    for (size_t i = 0; i < streams_.size(); ++i) {
        streams_[i]->open(streamPaths_[i], settings_.streamBufferMs, true);
        streams_[i]->setSimulateSlowDisk(settings_.simulateSlowDisk);
    }
}

void AudioSystem::applySettings(const AudioSettings& s) {
    const bool restart = needsRestart(settings_, s);
    const bool streamsChanged = settings_.streamBufferMs != s.streamBufferMs;
    if (restart) closeDevice();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        settings_ = s;
    }
    for (auto& st : streams_) st->setSimulateSlowDisk(s.simulateSlowDisk);
    if (restart) {
        if (streamsChanged) restartStreams();
        openDevice();
    }
}

int AudioSystem::addSound(SoundAsset&& a) {
    // The audio thread reads assets_ outside the lock, so pause it while the table changes.
    const bool wasRunning = deviceRunning_ && device_;
    if (wasRunning) ma_device_stop(static_cast<ma_device*>(device_));
    assets_.push_back(std::move(a));
    if (wasRunning) ma_device_start(static_cast<ma_device*>(device_));
    return int(assets_.size()) - 1;
}

int AudioSystem::addStream(const std::string& path) {
    auto st = std::make_unique<StreamSource>();
    if (!st->open(path, settings_.streamBufferMs, true)) return -1;
    st->setSimulateSlowDisk(settings_.simulateSlowDisk);
    const bool wasRunning = deviceRunning_ && device_;
    if (wasRunning) ma_device_stop(static_cast<ma_device*>(device_));
    streamPaths_.push_back(path);
    streamPtrs_.push_back(st.get());
    streams_.push_back(std::move(st));
    if (wasRunning) ma_device_start(static_cast<ma_device*>(device_));
    return int(streams_.size()) - 1;
}

VoiceHandle AudioSystem::allocVoice(const PlayParams& p, int sound, int stream) {
    // caller holds mutex_
    int best = -1;
    float bestScore = 1e30f;
    for (int i = 0; i < kMaxVoiceRecords; ++i) {
        const VoiceDesc& d = descs_[size_t(i)];
        const VoiceFeedback& f = feedback_[size_t(i)];
        const bool free = !d.active && (d.generation == 0 || (f.generation == d.generation && f.finished) ||
                                        f.generation != d.generation);
        if (free) { best = i; break; }
        // steal candidate: lowest priority, then quietest
        if (d.stream < 0) {
            const float score = float(d.priority) * 100.0f + f.audibility;
            if (score < bestScore && d.priority <= p.priority) { bestScore = score; best = i; }
        }
    }
    if (best < 0) return {};
    VoiceDesc& d = descs_[size_t(best)];
    const uint32_t gen = d.generation + 1;
    d = VoiceDesc{};
    d.generation = gen;
    d.active = true;
    d.sound = sound;
    d.stream = stream;
    d.bus = p.bus;
    d.is3D = p.is3D;
    d.loop = p.loop;
    d.volume = p.volume;
    d.pitch = p.pitch;
    d.priority = p.priority;
    d.position = p.position;
    d.velocity = p.velocity;
    d.refDistance = p.refDistance;
    d.maxDistance = p.maxDistance;
    d.reverbSend = p.reverbSend;
    d.occlusion = p.is3D ? occlusionFor(listener_.position, p.position) : 0.0f;
    captions_[size_t(best)] = p.caption;
    return {best, gen};
}

VoiceHandle AudioSystem::play(int sound, const PlayParams& p) {
    VoiceHandle h;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (sound < 0 || sound >= int(assets_.size())) return {};
        h = allocVoice(p, sound, -1);
    }
    if (h.valid() && p.caption) {
        showSubtitle("", p.caption, 2.5f, true, p.is3D ? &p.position : nullptr);
        if (p.is3D) events_.push_back({p.position, p.volume, 0.0f, p.caption});
    }
    return h;
}

VoiceHandle AudioSystem::playStream(int stream, const PlayParams& p) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (stream < 0 || stream >= int(streams_.size())) return {};
    return allocVoice(p, -1, stream);
}

VoiceHandle AudioSystem::playVoiceLine(int sound, const PlayParams& p, const std::string& speaker, const std::string& text,
                                       float seconds) {
    VoiceHandle h = play(sound, p);
    if (h.valid()) showSubtitle(speaker, text, seconds, false, p.is3D ? &p.position : nullptr);
    return h;
}

void AudioSystem::stop(VoiceHandle h) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (h.valid() && descs_[size_t(h.index)].generation == h.generation) descs_[size_t(h.index)].active = false;
}

bool AudioSystem::isPlaying(VoiceHandle h) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return h.valid() && descs_[size_t(h.index)].generation == h.generation && descs_[size_t(h.index)].active;
}

void AudioSystem::setVoicePosition(VoiceHandle h, const Vec3& pos, const Vec3& vel) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!h.valid() || descs_[size_t(h.index)].generation != h.generation) return;
    descs_[size_t(h.index)].position = pos;
    descs_[size_t(h.index)].velocity = vel;
}

void AudioSystem::setListener(const Listener& l) {
    std::lock_guard<std::mutex> lk(mutex_);
    listener_ = l;
}

void AudioSystem::addOccluderBox(const Vec3& mn, const Vec3& mx) { occluders_.emplace_back(mn, mx); }

float computeOcclusion(OcclusionMode mode, const Vec3& from, const Vec3& to, const std::vector<std::pair<Vec3, Vec3>>& boxes,
                       const std::function<float(const Vec3&, const Vec3&)>& rayQuery) {
    if (mode == OcclusionMode::Off) return 0.0f;
    auto blocked = [&](const Vec3& a, const Vec3& b) {
        float v = 0.0f;
        for (auto& box : boxes)
            if (segmentHitsBox(a, b, box.first, box.second)) { v = 1.0f; break; }
        if (rayQuery) v = std::max(v, rayQuery(a, b));
        return v;
    };
    if (mode == OcclusionMode::SingleRay) return blocked(from, to);
    // Multi-ray: centre ray + 4 rays to points offset around the source, giving
    // partial occlusion when a source is half behind a corner.
    Vec3 dir = to - from;
    const float len = std::max(length(dir), 1e-3f);
    dir = dir * (1.0f / len);
    Vec3 side = cross(dir, Vec3{0, 1, 0});
    if (length(side) < 1e-3f) side = Vec3{1, 0, 0};
    side = side * (1.0f / length(side));
    const Vec3 up = cross(side, dir);
    const float r = 0.6f;
    const Vec3 targets[5] = {to, to + side * r, to - side * r, to + up * r, to - up * r};
    float sum = 0.0f;
    for (const Vec3& t : targets) sum += blocked(from, t);
    return sum / 5.0f;
}

float AudioSystem::occlusionFor(const Vec3& from, const Vec3& to) const {
    return computeOcclusion(settings_.occlusion, from, to, occluders_, rayQuery_);
}

void AudioSystem::showSubtitle(const std::string& speaker, const std::string& text, float seconds, bool caption,
                               const Vec3* pos) {
    Subtitle s{speaker, text, seconds, caption, pos != nullptr, pos ? *pos : Vec3{}};
    subtitles_.push_back(std::move(s));
    if (subtitles_.size() > 4) subtitles_.erase(subtitles_.begin());
}

void AudioSystem::update(float dt) {
    auto t0 = Clock::now();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (int i = 0; i < kMaxVoiceRecords; ++i) {
            VoiceDesc& d = descs_[size_t(i)];
            const VoiceFeedback& f = feedback_[size_t(i)];
            if (d.active && f.generation == d.generation && f.finished) d.active = false;
        }
        // Occlusion rays are the expensive part: spread them over frames (round robin).
        const int budget = settings_.occlusion == OcclusionMode::MultiRay ? 24 : 64;
        int done = 0;
        for (int k = 0; k < kMaxVoiceRecords && done < budget; ++k) {
            occlusionCursor_ = (occlusionCursor_ + 1) % kMaxVoiceRecords;
            VoiceDesc& d = descs_[size_t(occlusionCursor_)];
            if (!d.active || !d.is3D) continue;
            d.occlusion = occlusionFor(listener_.position, d.position);
            ++done;
        }
        stats_.playingVoices = 0;
        for (auto& d : descs_) stats_.playingVoices += d.active ? 1 : 0;
    }
    const double occMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    stats_.occlusionMs = 0.9 * stats_.occlusionMs + 0.1 * occMs;

    // Background "Pause device": once the fade-out has finished, stop the device so
    // the audio thread costs 0% CPU; restart on focus.
    if (device_) {
        auto* dev = static_cast<ma_device*>(device_);
        if (!focused_ && settings_.background == BackgroundAudio::Pause && deviceRunning_ && counters_.backgroundGain <= 0.001f) {
            ma_device_stop(dev);
            deviceRunning_ = false;
        } else if ((focused_ || settings_.background != BackgroundAudio::Pause) && !deviceRunning_) {
            deviceRunning_ = ma_device_start(dev) == MA_SUCCESS;
        }
    }

    for (auto& s : subtitles_) s.remaining -= dt;
    subtitles_.erase(std::remove_if(subtitles_.begin(), subtitles_.end(), [](const Subtitle& s) { return s.remaining <= 0; }),
                     subtitles_.end());
    for (auto& e : events_) e.age += dt;
    events_.erase(std::remove_if(events_.begin(), events_.end(), [](const SoundEvent& e) { return e.age > 3.0f; }), events_.end());

    // ---- stats, refreshed twice a second ----
    statsTimer_ += dt;
    if (statsTimer_ < 0.5) return;
    MixerCounters c;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        c = counters_;
    }
    const double audioSec = double(c.framesRendered - lastCounters_.framesRendered) / std::max<uint32_t>(stats_.sampleRate, 1);
    double total = 0.0;
    for (int s = 0; s < kStageCount; ++s) {
        const double ns = double(c.stageNs[size_t(s)] - lastCounters_.stageNs[size_t(s)]);
        stats_.stageCpuPct[size_t(s)] = audioSec > 0 ? ns / (audioSec * 1e9) * 100.0 : 0.0;
        total += stats_.stageCpuPct[size_t(s)];
    }
    stats_.mixerCpuPct = total;
    stats_.callbackCpuPct = audioSec > 0 ? double(c.callbackNs - lastCounters_.callbackNs) / (audioSec * 1e9) * 100.0 : 0.0;
    double streamSec = 0.0;
    size_t streamBytes = 0;
    uint64_t underruns = 0;
    int bufferedMs = 0;
    for (auto& st : streams_) {
        streamSec += st->decodeSeconds();
        streamBytes += st->memoryBytes();
        underruns += st->underruns();
        bufferedMs = int(1000.0 * st->bufferedFrames() / std::max<uint32_t>(st->sampleRate(), 1));
    }
    stats_.streamCpuPct = (streamSec - lastStreamSec_) / statsTimer_ * 100.0;
    lastStreamSec_ = streamSec;
    stats_.streamUnderruns = underruns;
    stats_.streamBufferedMs = bufferedMs;
    stats_.mixerMem = c.memBytes;
    stats_.streamBytes = streamBytes;
    stats_.assetBytes = 0;
    for (auto& a : assets_) stats_.assetBytes += a.memoryBytes();
    stats_.totalBytes = stats_.assetBytes + stats_.streamBytes + stats_.deviceBufferBytes;
    for (size_t m : c.memBytes) stats_.totalBytes += m;
    stats_.realVoices = c.realVoices;
    stats_.virtualVoices = c.virtualVoices;
    stats_.peak = c.peak;
    stats_.rms = c.rms;
    stats_.gainReductionDb = c.gainReductionDb;
    stats_.deviceRunning = deviceRunning_;
    lastCounters_ = c;
    statsTimer_ = 0.0;
}

void AudioSystem::onAudio(void* out, unsigned frames) {
    const auto t0 = Clock::now();
    {
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        // If the game thread holds the lock we skip this sync and mix with last
        // block's state: never block the audio thread.
        if (lk.owns_lock()) mixer_.sync(settings_, listener_, focused_, descs_, feedback_, counters_);
    }
    mixer_.renderToFormat(out, deviceFormat_, int(frames));
    mixer_.addCallbackNs(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()));
}

}  // namespace gfx::audio
