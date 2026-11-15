#include "audio_dsp.h"

#include <algorithm>
#include <array>
#include <mutex>

namespace gfx::audio::dsp {

// ---- filters ---------------------------------------------------------------

void Biquad::setLowpass(float hz, float q, float sampleRate) {
    hz = clampf(hz, 10.0f, 0.45f * sampleRate);
    float w0 = 2.0f * kPi * hz / sampleRate;
    float cw = std::cos(w0), alpha = std::sin(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f - cw) * 0.5f / a0;
    b1 = (1.0f - cw) / a0;
    b2 = b0;
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
}

void HeadShadow::set(float alpha, float sampleRate) {
    // Bilinear transform of H(s) with s -> K(1 - z^-1)/(1 + z^-1), K = 2*fs.
    const float w0 = kSpeedOfSound / kHeadRadius;
    const float k = 2.0f * sampleRate / (2.0f * w0);
    const float d0 = 1.0f + k;
    b0 = (1.0f + alpha * k) / d0;
    b1 = (1.0f - alpha * k) / d0;
    a1 = (1.0f - k) / d0;
}

// ---- resampling ------------------------------------------------------------

namespace {
constexpr int kSincTaps = 8;
constexpr int kSincPhases = 512;
std::array<float, (kSincPhases + 1) * kSincTaps> gSinc;
std::once_flag gSincOnce;

void buildSinc() {
    const double cutoff = 0.92;  // fraction of Nyquist kept; leaves a small transition band
    for (int p = 0; p <= kSincPhases; ++p) {
        double frac = double(p) / kSincPhases;
        double sum = 0.0;
        double taps[kSincTaps];
        for (int k = 0; k < kSincTaps; ++k) {
            double x = double(k - (kSincTaps / 2 - 1)) - frac;  // taps at -3..+4 around floor(pos)
            double s = (std::abs(x) < 1e-9) ? 1.0 : std::sin(kPi * cutoff * x) / (kPi * cutoff * x);
            double w = 0.42 + 0.5 * std::cos(kPi * x / (kSincTaps / 2)) + 0.08 * std::cos(2 * kPi * x / (kSincTaps / 2));  // Blackman
            if (std::abs(x) >= kSincTaps / 2) w = 0.0;
            taps[k] = s * w;
            sum += taps[k];
        }
        for (int k = 0; k < kSincTaps; ++k) gSinc[p * kSincTaps + k] = float(taps[k] / sum);
    }
}
}  // namespace

void initSincTable() { std::call_once(gSincOnce, buildSinc); }

double resample(ResamplerQuality q, const float* src, double pos, double step, float* out, int frames) {
    switch (q) {
        case ResamplerQuality::Nearest:
            for (int i = 0; i < frames; ++i, pos += step) out[i] = src[int64_t(pos + 0.5)];
            break;
        case ResamplerQuality::Linear:
            for (int i = 0; i < frames; ++i, pos += step) {
                int64_t n = int64_t(pos);
                float t = float(pos - double(n));
                out[i] = src[n] + (src[n + 1] - src[n]) * t;
            }
            break;
        case ResamplerQuality::Cubic:  // Catmull-Rom / cubic Hermite, 4 taps
            for (int i = 0; i < frames; ++i, pos += step) {
                int64_t n = int64_t(pos);
                float t = float(pos - double(n));
                float xm1 = src[n - 1], x0 = src[n], x1 = src[n + 1], x2 = src[n + 2];
                float c1 = 0.5f * (x1 - xm1);
                float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                out[i] = ((c3 * t + c2) * t + c1) * t + x0;
            }
            break;
        case ResamplerQuality::Sinc8:
        default:
            for (int i = 0; i < frames; ++i, pos += step) {
                int64_t n = int64_t(pos);
                double frac = pos - double(n);
                const float* taps = &gSinc[int(frac * kSincPhases + 0.5) * kSincTaps];
                const float* s = src + n - (kSincTaps / 2 - 1);
                float acc = 0.0f;
                for (int k = 0; k < kSincTaps; ++k) acc += s[k] * taps[k];
                out[i] = acc;
            }
            break;
    }
    return pos;
}

// ---- spatial ---------------------------------------------------------------

SpeakerLayout makeLayout(int channels) {
    SpeakerLayout l;
    l.channels = channels;
    auto set = [&](std::initializer_list<float> az) {
        int i = 0;
        for (float a : az) l.azimuthDeg[i++] = a;
    };
    switch (channels) {
        case 1: set({0}); break;
        case 2: set({-30, 30}); break;
        case 4: set({-45, 45, -135, 135}); l.isRear[2] = l.isRear[3] = true; break;
        case 6: set({-30, 30, 0, 0, -110, 110}); l.lfeIndex = 3; l.isRear[4] = l.isRear[5] = true; break;
        case 8: set({-30, 30, 0, 0, -150, 150, -90, 90}); l.lfeIndex = 3;
                l.isRear[4] = l.isRear[5] = l.isRear[6] = l.isRear[7] = true; break;
        default:
            l.channels = 2; set({-30, 30}); break;
    }
    return l;
}

static float wrap360(float a) {
    a = std::fmod(a, 360.0f);
    return a < 0 ? a + 360.0f : a;
}

void vbapGains(const SpeakerLayout& layout, float azimuthDeg, float spread, float* gains) {
    int idx[8], n = 0;
    for (int c = 0; c < layout.channels; ++c) {
        gains[c] = 0.0f;
        if (c != layout.lfeIndex) idx[n++] = c;
    }
    if (n == 1) { gains[idx[0]] = 1.0f; return; }
    std::sort(idx, idx + n, [&](int a, int b) { return wrap360(layout.azimuthDeg[a]) < wrap360(layout.azimuthDeg[b]); });
    float az = wrap360(azimuthDeg);
    for (int i = 0; i < n; ++i) {
        int a = idx[i], b = idx[(i + 1) % n];
        float aa = wrap360(layout.azimuthDeg[a]), ab = wrap360(layout.azimuthDeg[b]);
        float span = wrap360(ab - aa);
        if (span == 0.0f) span = 360.0f;
        float off = wrap360(az - aa);
        if (off <= span) {
            float t = off / span;
            gains[a] = std::cos(t * kPi * 0.5f);
            gains[b] = std::sin(t * kPi * 0.5f);
            break;
        }
    }
    if (spread > 0.0f) {
        float even = 1.0f / std::sqrt(float(n)), power = 0.0f;
        for (int i = 0; i < n; ++i) {
            gains[idx[i]] = lerpf(gains[idx[i]], even, spread);
            power += gains[idx[i]] * gains[idx[i]];
        }
        float norm = 1.0f / std::sqrt(std::max(power, 1e-6f));
        for (int i = 0; i < n; ++i) gains[idx[i]] *= norm;
    }
}

void stereoPanGains(const SpeakerLayout& layout, float azimuthDeg, float* gains) {
    for (int c = 0; c < layout.channels; ++c) gains[c] = 0.0f;
    if (layout.channels == 1) { gains[0] = 1.0f; return; }
    float pan = clampf(std::sin(azimuthDeg * kPi / 180.0f), -1.0f, 1.0f);
    float theta = (pan + 1.0f) * kPi * 0.25f;
    gains[0] = std::cos(theta);
    gains[1] = std::sin(theta);
}

// ---- reverb ----------------------------------------------------------------

void Reverb::configure(ReverbQuality q, float sampleRate) {
    static const int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int kAp[4] = {556, 441, 341, 225};
    const int kSpread = 23;
    quality_ = q;
    combL_.clear(); combR_.clear(); apL_.clear(); apR_.clear();
    if (q == ReverbQuality::Off) return;
    const float scale = sampleRate / 44100.0f;
    const int combs = (q == ReverbQuality::High) ? 8 : 4;
    const int aps = (q == ReverbQuality::High) ? 4 : 2;
    const bool stereo = (q == ReverbQuality::High);
    auto len = [&](int base) { return std::max(1, int(base * scale)); };
    for (int i = 0; i < combs; ++i) {
        combL_.push_back({std::vector<float>(len(kComb[i]), 0.0f)});
        if (stereo) combR_.push_back({std::vector<float>(len(kComb[i] + kSpread), 0.0f)});
    }
    for (int i = 0; i < aps; ++i) {
        apL_.push_back({std::vector<float>(len(kAp[i]), 0.0f)});
        if (stereo) apR_.push_back({std::vector<float>(len(kAp[i] + kSpread), 0.0f)});
    }
}

void Reverb::setParams(float roomSize, float damping) {
    feedback_ = clampf(roomSize, 0.0f, 1.0f) * 0.28f + 0.7f;
    damp1_ = clampf(damping, 0.0f, 1.0f) * 0.4f;
    damp2_ = 1.0f - damp1_;
}

void Reverb::process(const float* in, float* outL, float* outR, int frames) {
    if (quality_ == ReverbQuality::Off) return;
    const float kFixedGain = 0.015f;
    const bool stereo = !combR_.empty();
    for (int i = 0; i < frames; ++i) {
        float x = in[i] * kFixedGain, l = 0.0f, r = 0.0f;
        for (auto& c : combL_) l += c.process(x, feedback_, damp1_, damp2_);
        for (auto& a : apL_) l = a.process(l);
        if (stereo) {
            for (auto& c : combR_) r += c.process(x, feedback_, damp1_, damp2_);
            for (auto& a : apR_) r = a.process(r);
        } else {
            r = l;
        }
        outL[i] += l;
        outR[i] += r;
    }
}

size_t Reverb::memoryBytes() const {
    size_t b = 0;
    for (auto* v : {&combL_, &combR_}) for (auto& c : *v) b += c.buf.capacity() * sizeof(float);
    for (auto* v : {&apL_, &apR_}) for (auto& a : *v) b += a.buf.capacity() * sizeof(float);
    return b;
}

// ---- dynamics --------------------------------------------------------------

void Compressor::setTimes(float attackMs, float releaseMs, float sampleRate) {
    attackCoef = std::exp(-1.0f / (attackMs * 0.001f * sampleRate));
    releaseCoef = std::exp(-1.0f / (releaseMs * 0.001f * sampleRate));
}

float Compressor::staticCurveDb(float levelDb) const {
    float over = levelDb - thresholdDb;
    float slope = 1.0f - 1.0f / ratio;
    if (2.0f * over < -kneeDb) return 0.0f;
    if (kneeDb > 0.0f && 2.0f * std::abs(over) <= kneeDb) {
        float t = over + kneeDb * 0.5f;
        return slope * t * t / (2.0f * kneeDb);
    }
    return slope * over;
}

float Compressor::process(float* x, int frames, int channels) {
    if (ratio <= 1.0f) { envDb = 0.0f; return 0.0f; }
    float maxGr = 0.0f;
    const float makeup = makeupDb;
    for (int i = 0; i < frames; ++i) {
        float* f = x + size_t(i) * channels;
        float peak = 0.0f;
        for (int c = 0; c < channels; ++c) peak = std::max(peak, std::abs(f[c]));
        float target = staticCurveDb(gainToDb(peak));
        float coef = target > envDb ? attackCoef : releaseCoef;
        envDb = target + (envDb - target) * coef;
        float g = dbToGain(makeup - envDb);
        for (int c = 0; c < channels; ++c) f[c] *= g;
        maxGr = std::max(maxGr, envDb);
    }
    return maxGr;
}

void configureDynamics(DynamicRange mode, float sr, Compressor& comp, Compressor& lim) {
    // Safety limiter is always on: -1 dBFS ceiling, fast attack.
    lim.thresholdDb = -1.0f; lim.ratio = 20.0f; lim.kneeDb = 0.0f; lim.makeupDb = 0.0f;
    lim.setTimes(0.5f, 80.0f, sr);
    switch (mode) {
        case DynamicRange::Wide:
            comp.ratio = 1.0f; comp.makeupDb = 0.0f; break;
        case DynamicRange::Medium:
            comp.thresholdDb = -20.0f; comp.ratio = 2.5f; comp.kneeDb = 6.0f; comp.makeupDb = 5.0f;
            comp.setTimes(10.0f, 200.0f, sr); break;
        case DynamicRange::Narrow:
        default:
            comp.thresholdDb = -32.0f; comp.ratio = 5.0f; comp.kneeDb = 8.0f; comp.makeupDb = 12.0f;
            comp.setTimes(3.0f, 150.0f, sr); break;
    }
}

}  // namespace gfx::audio::dsp
