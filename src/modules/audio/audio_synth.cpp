#include "audio_synth.h"

#include <algorithm>
#include <cmath>

#include "../../../third_party/miniaudio/miniaudio.h"

namespace gfx::audio::synth {

namespace {
constexpr float kTwoPi = 6.28318530718f;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2654435761u + 1u) {}
    float next() {  // -1..1
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
};

// Chamberlin state-variable filter, band-pass output.
struct Svf {
    float low = 0, band = 0;
    float bp(float x, float f, float q) {
        low += f * band;
        float high = x - low - q * band;
        band += f * high;
        return band;
    }
};

float svfCoef(float hz, uint32_t sr) { return 2.0f * std::sin(3.14159265f * std::min(hz, sr * 0.2f) / sr); }

void normalize(SoundAsset& a, float peakTarget) {
    float peak = 1e-6f;
    for (float v : a.data) peak = std::max(peak, std::abs(v));
    for (float& v : a.data) v *= peakTarget / peak;
}
}  // namespace

SoundAsset engineHum(uint32_t sr) {
    SoundAsset a{"engine_hum", sr, 1, {}};
    const size_t n = sr * 2;
    a.data.resize(n);
    Rng rng(7);
    float lp = 0;
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) / sr;
        float f = 80.0f;  // 160 whole cycles in 2 s -> seamless loop
        float v = 0;
        for (int h = 1; h <= 8; ++h) v += std::sin(kTwoPi * f * h * t) / h;
        lp += 0.05f * (rng.next() - lp);
        a.data[i] = v * (0.8f + 0.2f * std::sin(kTwoPi * 4.0f * t)) + lp * 0.6f;
    }
    normalize(a, 0.7f);
    return a;
}

SoundAsset carEngine(uint32_t sr) {
    SoundAsset a{"car_engine", sr, 1, {}};
    const size_t n = sr;
    a.data.resize(n);
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) / sr;
        float v = 0;
        for (int h = 1; h <= 12; ++h) v += std::sin(kTwoPi * 110.0f * h * t + h) * (h % 2 ? 1.0f : 0.6f) / h;
        a.data[i] = std::tanh(1.8f * v);
    }
    normalize(a, 0.8f);
    return a;
}

SoundAsset footstep(uint32_t sr, uint32_t seed) {
    SoundAsset a{"footstep", sr, 1, {}};
    const size_t n = size_t(sr * 0.18f);
    a.data.resize(n);
    Rng rng(seed);
    Svf svf;
    float fc = svfCoef(900.0f + 300.0f * (seed % 3), sr);
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) / sr;
        float env = std::exp(-t * 35.0f) * std::min(1.0f, t * 800.0f);
        a.data[i] = svf.bp(rng.next(), fc, 0.7f) * env + std::sin(kTwoPi * 70.0f * t) * env * 0.5f;
    }
    normalize(a, 0.6f);
    return a;
}

SoundAsset explosion(uint32_t sr) {
    SoundAsset a{"explosion", sr, 1, {}};
    const size_t n = size_t(sr * 2.0f);
    a.data.resize(n);
    Rng rng(99);
    float lp = 0;
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) / sr;
        float env = std::exp(-t * 2.5f) * std::min(1.0f, t * 300.0f);
        float k = 0.02f + 0.5f * std::exp(-t * 6.0f);  // filter closes as it decays
        lp += k * (rng.next() - lp);
        float boom = std::sin(kTwoPi * (45.0f + 60.0f * std::exp(-t * 10.0f)) * t) * std::exp(-t * 4.0f);
        a.data[i] = (lp * 2.5f + boom) * env;
    }
    normalize(a, 0.95f);
    return a;
}

SoundAsset wind(uint32_t sr) {
    SoundAsset a{"wind", sr, 1, {}};
    const size_t n = sr * 4, fade = sr / 2;
    std::vector<float> raw(n + fade);
    Rng rng(3);
    Svf svf;
    for (size_t i = 0; i < raw.size(); ++i) {
        float t = float(i) / sr;
        float f = 400.0f + 250.0f * std::sin(kTwoPi * 0.3f * t) + 120.0f * std::sin(kTwoPi * 0.77f * t);
        raw[i] = svf.bp(rng.next(), svfCoef(f, sr), 0.3f);
    }
    a.data.resize(n);
    for (size_t i = 0; i < n; ++i) {  // crossfade the tail into the head for a seamless loop
        float v = raw[i];
        if (i < fade) {
            float w = float(i) / fade;
            v = raw[i] * w + raw[n + i] * (1.0f - w);
        }
        a.data[i] = v;
    }
    normalize(a, 0.5f);
    return a;
}

SoundAsset uiClick(uint32_t sr) {
    SoundAsset a{"ui_click", sr, 1, {}};
    const size_t n = size_t(sr * 0.04f);
    a.data.resize(n);
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) / sr;
        a.data[i] = std::sin(kTwoPi * 2200.0f * t) * std::exp(-t * 150.0f);
    }
    normalize(a, 0.5f);
    return a;
}

SoundAsset voiceLine(float seconds, uint32_t seed, uint32_t sr) {
    // Glottal pulse train through three formant band-passes that hop between
    // vowel targets every syllable. Sounds like distant mumbling speech.
    static const float kVowels[5][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}, {570, 840, 2410}};
    SoundAsset a{"voice_line", sr, 1, {}};
    const size_t n = size_t(sr * seconds);
    a.data.resize(n);
    Rng rng(seed);
    Svf f1, f2, f3;
    float phase = 0;
    const float syllable = 0.16f;
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) / sr;
        int syl = int(t / syllable);
        float st = std::fmod(t, syllable) / syllable;
        const float* v = kVowels[(syl * 7 + seed) % 5];
        float pitch = 115.0f + 25.0f * std::sin(t * 2.1f + seed) - 20.0f * (t / seconds);
        phase += pitch / sr;
        if (phase >= 1.0f) phase -= 1.0f;
        float glottal = (phase < 0.4f ? std::sin(3.14159265f * phase / 0.4f) : 0.0f) + 0.05f * rng.next();
        float env = std::sin(3.14159265f * st);
        env *= ((syl * 13 + seed) % 6 == 0) ? 0.05f : 1.0f;  // occasional pause between words
        float y = f1.bp(glottal, svfCoef(v[0], sr), 0.25f) + 0.6f * f2.bp(glottal, svfCoef(v[1], sr), 0.2f) +
                  0.3f * f3.bp(glottal, svfCoef(v[2], sr), 0.2f);
        a.data[i] = y * env;
    }
    normalize(a, 0.7f);
    return a;
}

SoundAsset musicLoop(uint32_t sr) {
    // 4 chords x 6 s, pad + bass + hi-hat. Ends on the same phase it starts.
    static const float kChords[4][3] = {{220.0f, 261.63f, 329.63f}, {174.61f, 220.0f, 261.63f}, {130.81f, 164.81f, 196.0f}, {196.0f, 246.94f, 293.66f}};
    static const float kBass[4] = {55.0f, 43.65f, 65.41f, 49.0f};
    SoundAsset a{"music_loop", sr, 2, {}};
    const float barLen = 6.0f;
    const size_t n = size_t(sr * barLen * 4);
    a.data.resize(n * 2);
    Rng rng(11);
    float hatLp = 0;
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) / sr;
        int bar = int(t / barLen) % 4;
        float bt = std::fmod(t, barLen);
        float xf = std::min(1.0f, bt / 0.3f) * std::min(1.0f, (barLen - bt) / 0.3f);
        float l = 0, r = 0;
        for (int k = 0; k < 3; ++k) {
            float f = kChords[bar][k];
            l += std::sin(kTwoPi * f * t) + 0.3f * std::sin(kTwoPi * f * 2.003f * t);
            r += std::sin(kTwoPi * f * 1.002f * t) + 0.3f * std::sin(kTwoPi * f * 1.997f * t);
        }
        float beat = std::fmod(t, 0.5f);
        float bass = std::sin(kTwoPi * kBass[bar] * t) * std::exp(-beat * 3.0f);
        float hatT = std::fmod(t + 0.25f, 0.5f);
        hatLp += 0.6f * (rng.next() - hatLp);
        float hat = (rng.next() - hatLp) * std::exp(-hatT * 60.0f) * 0.25f;
        a.data[i * 2 + 0] = (0.15f * l * xf + 0.5f * bass + hat * 0.8f);
        a.data[i * 2 + 1] = (0.15f * r * xf + 0.5f * bass + hat * 1.2f);
    }
    normalize(a, 0.6f);
    return a;
}

bool writeWav(const std::string& path, const SoundAsset& a) {
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, ma_uint32(a.channels), a.sampleRate);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) return false;
    std::vector<int16_t> pcm(a.data.size());
    for (size_t i = 0; i < a.data.size(); ++i) pcm[i] = int16_t(std::clamp(a.data[i], -1.0f, 1.0f) * 32767.0f);
    ma_uint64 written = 0;
    ma_encoder_write_pcm_frames(&enc, pcm.data(), a.frames(), &written);
    ma_encoder_uninit(&enc);
    return written == a.frames();
}

}  // namespace gfx::audio::synth
