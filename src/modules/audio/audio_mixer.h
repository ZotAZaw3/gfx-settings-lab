// The software mixer: voices -> resample -> filter -> spatialize -> buses ->
// reverb -> bass management -> dynamics -> master -> device format.
// Runs on the audio device thread (AudioSystem) or offline (tools/audio_bench).
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "audio_dsp.h"
#include "audio_settings.h"
#include "audio_synth.h"

namespace gfx::audio {

class StreamSource;

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

struct Listener {
    Vec3 position, forward{0, 0, -1}, up{0, 1, 0}, velocity;
};

constexpr int kMaxVoiceRecords = 512;   // playing sounds tracked (real + virtual)
constexpr int kMaxRealVoices = 256;     // upper bound for the "max voices" setting
constexpr int kMaxChannels = 8;
constexpr int kChunkFrames = 256;       // internal processing block

enum class Stage : uint8_t { VoiceSelect, Resample, Filter, Spatialize, BusMix, Reverb, BassManagement, Dynamics, Master, Output, Count };
constexpr int kStageCount = static_cast<int>(Stage::Count);
const char* toString(Stage s);

enum class MemCategory : uint8_t { VoicePool, MixBuffers, Reverb, Hrtf, Count };
constexpr int kMemCategoryCount = static_cast<int>(MemCategory::Count);
const char* toString(MemCategory c);

// What the game thread wants a voice to do. Written under AudioSystem's lock.
struct VoiceDesc {
    uint32_t generation = 0;   // bumped on every play() so a reused slot restarts cleanly
    bool active = false;       // false = stop (fades out)
    int sound = -1;            // index into the asset table, or -1
    int stream = -1;           // index into the stream table, or -1
    Bus bus = Bus::Sfx;
    bool is3D = true;
    bool loop = false;
    float volume = 1.0f, pitch = 1.0f;
    int priority = 128;        // 0..255, higher wins voice-limit fights
    Vec3 position, velocity;
    float refDistance = 1.0f, maxDistance = 60.0f;
    float occlusion = 0.0f;    // 0..1, computed by AudioSystem::update on the game thread
    float reverbSend = 1.0f;
};

struct VoiceFeedback {
    uint32_t generation = 0;
    bool finished = false;
    bool real = false;
    float audibility = 0.0f;
};

// Cumulative counters, copied out under the shared lock.
struct MixerCounters {
    std::array<uint64_t, kStageCount> stageNs{};
    uint64_t framesRendered = 0;
    uint64_t callbackNs = 0;     // whole device callback incl. sync
    uint64_t callbacks = 0;
    int realVoices = 0, virtualVoices = 0;
    std::array<size_t, kMemCategoryCount> memBytes{};
    std::array<float, kMaxChannels> peak{}, rms{};
    float gainReductionDb = 0.0f;
    float backgroundGain = 1.0f;
};

class Mixer {
public:
    Mixer();
    // Allocates everything for this rate/layout/settings. Never call while rendering.
    void configure(uint32_t sampleRate, int channels, const AudioSettings& s);
    void setAssets(const std::vector<SoundAsset>* assets) { assets_ = assets; }
    void setStreams(const std::vector<StreamSource*>* streams) { streams_ = streams; }

    // Copies live settings / listener / voice descs in and feedback/counters out.
    void sync(const AudioSettings& s, const Listener& l, bool focused,
              std::array<VoiceDesc, kMaxVoiceRecords>& descs,
              std::array<VoiceFeedback, kMaxVoiceRecords>& feedback, MixerCounters& counters);

    // Renders interleaved float in channels() channels.
    void render(float* out, int frames);
    // Renders and converts to the device format (f32 / s16 / s24 packed), with optional TPDF dither.
    void renderToFormat(void* out, SampleFormatSetting fmt, int frames);

    uint32_t sampleRate() const { return sampleRate_; }
    int channels() const { return layout_.channels; }
    const dsp::SpeakerLayout& layout() const { return layout_; }
    const MixerCounters& counters() const { return counters_; }
    void addCallbackNs(uint64_t ns) { counters_.callbackNs += ns; counters_.callbacks++; }
    bool backgroundFadeDone() const { return bgGain_ <= 0.0005f; }

private:
    struct RealSlot {
        int owner = -1;
        std::array<std::vector<float>, 2> buf;   // source frames, planar, with history
        int count = 0;                            // valid frames in buf
        int64_t base = 0;                         // absolute source frame of buf[0]
        int64_t fetchCursor = 0;                  // next absolute source frame to fetch
        std::array<std::vector<float>, 2> tmp;    // resampled output for this chunk
        dsp::OnePoleLP lp[2];
        float prevGains[kMaxChannels]{};
        float prevSend = 0.0f;
        bool demoting = false;
        // HRTF state (allocated only when HRTF is selected)
        std::vector<float> delay;
        int delayW = 0;
        float itdL = 0, itdR = 0;
        dsp::HeadShadow shadowL, shadowR;
        dsp::OnePoleLP rearL, rearR;
    };
    struct VoiceRuntime {
        uint32_t generation = 0;
        bool playing = false, finished = false, stopping = false;
        double srcPos = 0.0;      // absolute source frame position (playhead)
        int slot = -1;
        float audibility = 0.0f;
        float dopplerPitch = 1.0f;
        float occlusion = 0.0f;
    };

    void selectVoices();
    void renderChunk(float* out, int frames);
    void renderVoice(int index, int frames);
    int fetch(RealSlot& slot, const VoiceDesc& d, int64_t needUpTo);
    void releaseSlot(int index);
    float attenuation(const VoiceDesc& d, float dist) const;
    void updateMemory();

    // configuration
    uint32_t sampleRate_ = 48000;
    dsp::SpeakerLayout layout_;
    AudioSettings settings_;
    const std::vector<SoundAsset>* assets_ = nullptr;
    const std::vector<StreamSource*>* streams_ = nullptr;
    bool hrtfAllocated_ = false;

    // game state copy
    Listener listener_;
    bool focused_ = true;
    std::array<VoiceDesc, kMaxVoiceRecords> descs_{};
    std::array<VoiceRuntime, kMaxVoiceRecords> rt_{};

    // pools and buffers
    std::vector<RealSlot> slots_;
    std::vector<int> freeSlots_;
    std::vector<float> busBuf_;      // [bus][channel][kChunkFrames]
    std::vector<float> sendBuf_;     // reverb send, mono
    std::vector<float> wetL_, wetR_;
    std::vector<float> master_;      // [channel][kChunkFrames]
    std::vector<float> interleaved_; // scratch for renderToFormat
    std::vector<float> fetchScratch_; // interleaved stream reads
    std::array<float, kBusCount> busGain_{};
    std::vector<int> order_;

    dsp::Reverb reverb_;
    dsp::Biquad lfe1_, lfe2_;
    dsp::Compressor comp_, limiter_;
    dsp::Dither dither_;
    float bgGain_ = 1.0f, masterGain_ = 1.0f;
    MixerCounters counters_;
    int chunkCounter_ = 0;
};

}  // namespace gfx::audio
