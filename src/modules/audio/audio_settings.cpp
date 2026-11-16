#include "audio_settings.h"

#include <cstddef>

namespace gfx::audio {

namespace {
template <typename E, size_t N>
const char* pick(E v, const char* const (&names)[N]) {
    size_t i = static_cast<size_t>(v);
    return i < N ? names[i] : "?";
}
}  // namespace

const char* toString(Bus v) { static const char* const n[] = {"Music", "Sound effects", "Voice / dialogue", "Ambience", "Interface"}; return pick(v, n); }
const char* toString(SpeakerConfig v) { static const char* const n[] = {"Auto (device)", "Mono", "Stereo speakers", "Headphones", "Quadraphonic", "5.1 surround", "7.1 surround"}; return pick(v, n); }
const char* toString(SampleRateSetting v) { static const char* const n[] = {"Device default", "22.05 kHz", "44.1 kHz", "48 kHz", "96 kHz"}; return pick(v, n); }
const char* toString(SampleFormatSetting v) { static const char* const n[] = {"16-bit integer", "24-bit integer", "32-bit float"}; return pick(v, n); }
const char* toString(ResamplerQuality v) { static const char* const n[] = {"Nearest (lowest)", "Linear", "Cubic Hermite", "Windowed sinc, 8 tap"}; return pick(v, n); }
const char* toString(SpatialMode v) { static const char* const n[] = {"Stereo pan", "Speaker panning (VBAP)", "HRTF (binaural headphones)"}; return pick(v, n); }
const char* toString(DynamicRange v) { static const char* const n[] = {"Wide (home theater)", "Medium (TV)", "Narrow (night mode)"}; return pick(v, n); }
const char* toString(ReverbQuality v) { static const char* const n[] = {"Off", "Low (mono, 4 comb)", "High (stereo, 8 comb)"}; return pick(v, n); }
const char* toString(OcclusionMode v) { static const char* const n[] = {"Off", "Single ray", "Multi ray (5)"}; return pick(v, n); }
const char* toString(AttenuationModel v) { static const char* const n[] = {"None", "Linear", "Inverse distance", "Exponential"}; return pick(v, n); }
const char* toString(BackgroundAudio v) { static const char* const n[] = {"Keep playing", "Duck volume", "Mute", "Pause device"}; return pick(v, n); }
const char* toString(QualityPreset v) { static const char* const n[] = {"Low", "Medium", "High", "Ultra", "Custom"}; return pick(v, n); }

uint32_t sampleRateHz(SampleRateSetting v) {
    switch (v) {
        case SampleRateSetting::Hz22050: return 22050;
        case SampleRateSetting::Hz44100: return 44100;
        case SampleRateSetting::Hz48000: return 48000;
        case SampleRateSetting::Hz96000: return 96000;
        default: return 0;
    }
}

int speakerChannels(SpeakerConfig v) {
    switch (v) {
        case SpeakerConfig::Mono: return 1;
        case SpeakerConfig::Stereo:
        case SpeakerConfig::Headphones: return 2;
        case SpeakerConfig::Quad: return 4;
        case SpeakerConfig::Surround51: return 6;
        case SpeakerConfig::Surround71: return 8;
        default: return 0;
    }
}

int bytesPerSample(SampleFormatSetting v) {
    switch (v) {
        case SampleFormatSetting::S16: return 2;
        case SampleFormatSetting::S24: return 3;
        default: return 4;
    }
}

void applyQualityPreset(AudioSettings& s, QualityPreset p) {
    s.quality = p;
    switch (p) {
        case QualityPreset::Low:
            s.maxVoices = 24; s.resampler = ResamplerQuality::Linear;
            s.reverb = ReverbQuality::Low; s.occlusion = OcclusionMode::SingleRay; break;
        case QualityPreset::Medium:
            s.maxVoices = 48; s.resampler = ResamplerQuality::Cubic;
            s.reverb = ReverbQuality::Low; s.occlusion = OcclusionMode::SingleRay; break;
        case QualityPreset::High:
            s.maxVoices = 96; s.resampler = ResamplerQuality::Cubic;
            s.reverb = ReverbQuality::High; s.occlusion = OcclusionMode::MultiRay; break;
        case QualityPreset::Ultra:
            s.maxVoices = 192; s.resampler = ResamplerQuality::Sinc8;
            s.reverb = ReverbQuality::High; s.occlusion = OcclusionMode::MultiRay; break;
        default: break;
    }
}

bool needsRestart(const AudioSettings& a, const AudioSettings& b) {
    return a.deviceIndex != b.deviceIndex || a.speakers != b.speakers ||
           a.sampleRate != b.sampleRate || a.format != b.format ||
           a.bufferFrames != b.bufferFrames || a.periods != b.periods ||
           a.maxVoices != b.maxVoices || a.reverb != b.reverb ||
           (a.spatial == SpatialMode::HRTF) != (b.spatial == SpatialMode::HRTF) ||
           a.streamBufferMs != b.streamBufferMs;
}

}  // namespace gfx::audio
