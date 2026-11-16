// Small DSP building blocks used by the mixer. Everything here is written from
// scratch (no library DSP) so the docs can point at the exact code behind each
// setting. All processing is 32-bit float.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "audio_settings.h"

namespace gfx::audio::dsp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSpeedOfSound = 343.0f;   // m/s
constexpr float kHeadRadius = 0.0875f;    // m, average adult head

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return 20.0f * std::log10(g > 1e-9f ? g : 1e-9f); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ---- filters -------------------------------------------------------------

// One-pole low-pass: y += a * (x - y). 1 multiply-add per sample.
struct OnePoleLP {
    float a = 1.0f, z = 0.0f;
    void setCutoff(float hz, float sampleRate) {
        hz = clampf(hz, 10.0f, 0.49f * sampleRate);
        a = 1.0f - std::exp(-2.0f * kPi * hz / sampleRate);
    }
    float process(float x) { z += a * (x - z); return z; }
};

// RBJ cookbook biquad (used for the LFE crossover).
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void setLowpass(float hz, float q, float sampleRate);
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Brown & Duda (1998) spherical-head shadow filter, one pole / one zero:
// H(s) = (1 + alpha*s/(2*w0)) / (1 + s/(2*w0)),  w0 = c / headRadius.
// alpha ~2 boosts highs on the near ear, ~0.1 cuts them on the far ear.
struct HeadShadow {
    float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    void set(float alpha, float sampleRate);
    float process(float x) {
        float y = b0 * x + b1 * x1 - a1 * y1;
        x1 = x; y1 = y;
        return y;
    }
};

// ---- resampling ------------------------------------------------------------

// Reads src (contiguous, mono) at fractional positions pos, pos+step, ...
// The caller guarantees `kResampleTapsBefore` frames before floor(pos) and
// `kResampleTapsAfter` after the last read are valid.
constexpr int kResampleTapsBefore = 4;
constexpr int kResampleTapsAfter = 5;
void initSincTable();
double resample(ResamplerQuality q, const float* src, double pos, double step, float* out, int frames);

// ---- spatial ---------------------------------------------------------------

struct SpeakerLayout {
    int channels = 2;
    int lfeIndex = -1;        // -1 if no LFE
    float azimuthDeg[8]{};    // per channel, 0 = front, +90 = right
    bool isRear[8]{};         // used to spread reverb return
};
SpeakerLayout makeLayout(int channels);

// 2D VBAP: pairwise constant-power panning between the two speakers that
// bracket the source azimuth. `spread` (0..1) blends toward all speakers.
void vbapGains(const SpeakerLayout& layout, float azimuthDeg, float spread, float* gains);
// Simple L/R pan law used when spatialization is "stereo pan".
void stereoPanGains(const SpeakerLayout& layout, float azimuthDeg, float* gains);

// ---- reverb (Freeverb topology, Jezar at Dreampoint, public domain) --------

class Reverb {
public:
    void configure(ReverbQuality q, float sampleRate);
    void setParams(float roomSize, float damping);
    // in: mono send. outL/outR are *added to*.
    void process(const float* in, float* outL, float* outR, int frames);
    size_t memoryBytes() const;
    ReverbQuality quality() const { return quality_; }

private:
    struct Comb {
        std::vector<float> buf; int idx = 0; float store = 0;
        float process(float x, float feedback, float damp1, float damp2) {
            float out = buf[idx];
            store = out * damp2 + store * damp1;
            buf[idx] = x + store * feedback;
            if (++idx >= (int)buf.size()) idx = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf; int idx = 0;
        float process(float x) {
            float b = buf[idx];
            float out = -x + b;
            buf[idx] = x + b * 0.5f;
            if (++idx >= (int)buf.size()) idx = 0;
            return out;
        }
    };
    ReverbQuality quality_ = ReverbQuality::Off;
    std::vector<Comb> combL_, combR_;
    std::vector<Allpass> apL_, apR_;
    float feedback_ = 0.84f, damp1_ = 0.2f, damp2_ = 0.8f;
};

// ---- dynamics --------------------------------------------------------------

// Feed-forward, stereo-linked peak compressor with soft knee.
struct Compressor {
    float thresholdDb = 0, ratio = 1, kneeDb = 6, makeupDb = 0;
    float attackCoef = 0, releaseCoef = 0;
    float envDb = 0;              // current gain reduction (>= 0)
    void setTimes(float attackMs, float releaseMs, float sampleRate);
    float staticCurveDb(float levelDb) const;  // returns gain reduction in dB
    // Processes interleaved audio in place, returns max gain reduction in dB.
    float process(float* interleaved, int frames, int channels);
};

void configureDynamics(DynamicRange mode, float sampleRate, Compressor& comp, Compressor& limiter);

// ---- output ----------------------------------------------------------------

struct Dither {
    uint32_t state = 0x12345678u;
    float uniform() {  // xorshift32 -> [0,1)
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state >> 8) * (1.0f / 16777216.0f);
    }
    float tpdf() { return uniform() - uniform(); }  // triangular, -1..1 LSB
};

}  // namespace gfx::audio::dsp
