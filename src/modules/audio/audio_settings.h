// Audio settings for the gfx-settings-lab prototype.
// Every field here is a user-facing option in the "Audio" tab of the settings
// menu, and each one has a matching docs/audio_*.md page (see docs/audio_overview.md).
#pragma once

#include <array>
#include <cstdint>

namespace gfx::audio {

// ---------------------------------------------------------------- enums ----

enum class Bus : uint8_t { Music = 0, Sfx, Voice, Ambient, Ui, Count };
constexpr int kBusCount = static_cast<int>(Bus::Count);

enum class SpeakerConfig : uint8_t { Auto, Mono, Stereo, Headphones, Quad, Surround51, Surround71, Count };
enum class SampleRateSetting : uint8_t { Device, Hz22050, Hz44100, Hz48000, Hz96000, Count };
enum class SampleFormatSetting : uint8_t { S16, S24, F32, Count };
enum class ResamplerQuality : uint8_t { Nearest, Linear, Cubic, Sinc8, Count };
enum class SpatialMode : uint8_t { StereoPan, SpeakerPanning, HRTF, Count };
enum class DynamicRange : uint8_t { Wide, Medium, Narrow, Count };
enum class ReverbQuality : uint8_t { Off, Low, High, Count };
enum class OcclusionMode : uint8_t { Off, SingleRay, MultiRay, Count };
enum class AttenuationModel : uint8_t { None, Linear, Inverse, Exponential, Count };
enum class BackgroundAudio : uint8_t { KeepPlaying, Duck, Mute, Pause, Count };
enum class QualityPreset : uint8_t { Low, Medium, High, Ultra, Custom, Count };

const char* toString(Bus v);
const char* toString(SpeakerConfig v);
const char* toString(SampleRateSetting v);
const char* toString(SampleFormatSetting v);
const char* toString(ResamplerQuality v);
const char* toString(SpatialMode v);
const char* toString(DynamicRange v);
const char* toString(ReverbQuality v);
const char* toString(OcclusionMode v);
const char* toString(AttenuationModel v);
const char* toString(BackgroundAudio v);
const char* toString(QualityPreset v);

uint32_t sampleRateHz(SampleRateSetting v);   // 0 = use the device's native rate
int speakerChannels(SpeakerConfig v);         // 0 = use the device's native layout
int bytesPerSample(SampleFormatSetting v);

// ------------------------------------------------------------- settings ----

struct AudioSettings {
    // Volume & mixing (docs/audio_volume_mixing.md)
    float masterVolume = 0.8f;                                   // linear gain 0..1
    std::array<float, kBusCount> busVolume{0.6f, 1.0f, 1.0f, 0.7f, 0.6f};
    bool  muteAll = false;

    // Output device (docs/audio_output_device.md)
    int   deviceIndex = -1;                                      // -1 = OS default device

    // Speaker configuration (docs/audio_speaker_config.md)
    SpeakerConfig speakers = SpeakerConfig::Auto;
    float lfeCrossoverHz = 80.0f;                                // bass management for 5.1/7.1

    // Sample rate & bit depth (docs/audio_sample_rate_bit_depth.md)
    SampleRateSetting   sampleRate = SampleRateSetting::Hz48000;
    SampleFormatSetting format     = SampleFormatSetting::F32;
    bool dither = true;                                          // TPDF dither when going to 16/24-bit

    // Audio quality (docs/audio_quality_voices.md, docs/audio_resampling.md)
    QualityPreset    quality   = QualityPreset::High;
    int              maxVoices = 96;                             // real (mixed) voices; the rest go virtual
    ResamplerQuality resampler = ResamplerQuality::Cubic;

    // 3D / spatial audio (docs/audio_spatial_hrtf.md)
    SpatialMode spatial = SpatialMode::SpeakerPanning;

    // Distance attenuation (docs/audio_distance_attenuation.md)
    AttenuationModel attenuation = AttenuationModel::Inverse;
    float rolloff = 1.0f;
    bool  airAbsorption = true;

    // Doppler (docs/audio_doppler.md)
    bool  doppler = true;
    float dopplerFactor = 1.0f;

    // Occlusion (docs/audio_occlusion.md)
    OcclusionMode occlusion = OcclusionMode::MultiRay;

    // Reverb (docs/audio_reverb.md)
    ReverbQuality reverb = ReverbQuality::High;
    float reverbWet = 0.35f;
    float roomSize  = 0.75f;
    float damping   = 0.5f;

    // Dynamic range / night mode (docs/audio_dynamic_range.md)
    DynamicRange dynamicRange = DynamicRange::Wide;

    // Buffer size / latency (docs/audio_buffer_latency.md)
    int bufferFrames = 480;                                      // period size in frames
    int periods = 2;

    // Streaming buffer (docs/audio_streaming_buffer.md)
    int  streamBufferMs = 500;
    bool simulateSlowDisk = false;                               // debug: makes underruns visible

    // Background behaviour (docs/audio_background.md)
    BackgroundAudio background = BackgroundAudio::Mute;
    float duckVolume = 0.2f;

    // Accessibility (docs/audio_accessibility.md, docs/audio_subtitles.md)
    bool  monoAudio = false;
    float balance = 0.0f;                                        // -1 left .. +1 right
    bool  subtitles = true;
    bool  closedCaptions = false;                                // [explosion], [footsteps] ...
    bool  speakerNames = true;
    float subtitleScale = 1.0f;
    float subtitleBackground = 0.6f;
    bool  soundVisualization = false;                            // on-screen direction indicators

    bool operator==(const AudioSettings&) const = default;
};

// Applies the voice count / resampler / reverb / occlusion bundle for a preset.
void applyQualityPreset(AudioSettings& s, QualityPreset p);

// True if going from a to b needs the device (and mixer buffers) rebuilt.
bool needsRestart(const AudioSettings& a, const AudioSettings& b);

}  // namespace gfx::audio
