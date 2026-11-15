// AudioSystem: owns the audio device, assets, streams and mixer, and is the
// game-facing API (play/stop/3D params/listener/subtitles). Engine-agnostic:
// the gfx::Module wrapper lives in audio_module.cpp.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio_mixer.h"
#include "audio_settings.h"
#include "audio_stream.h"
#include "audio_synth.h"

namespace gfx::audio {

struct VoiceHandle {
    int index = -1;
    uint32_t generation = 0;
    bool valid() const { return index >= 0; }
};

struct PlayParams {
    Bus bus = Bus::Sfx;
    bool is3D = true, loop = false;
    float volume = 1.0f, pitch = 1.0f;
    int priority = 128;
    Vec3 position, velocity;
    float refDistance = 1.0f, maxDistance = 60.0f;
    float reverbSend = 1.0f;
    const char* caption = nullptr;   // closed caption, e.g. "[Explosion]"
};

struct Subtitle {
    std::string speaker, text;
    float remaining = 0.0f;
    bool caption = false;            // sound-effect caption rather than dialogue
    bool hasPosition = false;
    Vec3 position;
};

struct SoundEvent {                  // for the sound visualization overlay
    Vec3 position;
    float loudness = 1.0f, age = 0.0f;
    std::string label;
};

struct AudioStats {
    std::array<double, kStageCount> stageCpuPct{};  // % of one core, audio thread
    double mixerCpuPct = 0.0;                       // sum of stages
    double callbackCpuPct = 0.0;                    // whole device callback
    double streamCpuPct = 0.0;                      // decoder thread(s)
    double occlusionMs = 0.0;                       // game thread, per frame
    std::array<size_t, kMemCategoryCount> mixerMem{};
    size_t assetBytes = 0, streamBytes = 0, deviceBufferBytes = 0, totalBytes = 0;
    int realVoices = 0, virtualVoices = 0, playingVoices = 0;
    uint64_t streamUnderruns = 0;
    int streamBufferedMs = 0;
    std::array<float, kMaxChannels> peak{}, rms{};
    int channels = 2;
    uint32_t sampleRate = 0;
    int periodFrames = 0, periods = 0;
    double bufferLatencyMs = 0.0;
    float gainReductionDb = 0.0f;
    std::string backend, device, format;
    bool deviceRunning = false;
};

// Occlusion ray test used by AudioSystem::update (exposed for the benchmark).
float computeOcclusion(OcclusionMode mode, const Vec3& from, const Vec3& to,
                       const std::vector<std::pair<Vec3, Vec3>>& boxes,
                       const std::function<float(const Vec3&, const Vec3&)>& rayQuery = nullptr);

// Lists playback devices without opening one (used to build the menu options).
std::vector<std::string> enumerateOutputDevices();

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    // cacheDir: where generated WAVs for streaming are written.
    bool init(const AudioSettings& s, const std::string& cacheDir);
    void shutdown();

    void applySettings(const AudioSettings& s);   // restarts the device only when needed
    const AudioSettings& settings() const { return settings_; }
    std::vector<std::string> outputDevices() const { return deviceNames_; }

    int addSound(SoundAsset&& a);                 // returns sound id
    int addStream(const std::string& path);       // returns stream id (or -1)
    VoiceHandle play(int sound, const PlayParams& p);
    VoiceHandle playStream(int stream, const PlayParams& p);
    VoiceHandle playVoiceLine(int sound, const PlayParams& p, const std::string& speaker, const std::string& text, float seconds);
    void stop(VoiceHandle h);
    bool isPlaying(VoiceHandle h) const;
    void setVoicePosition(VoiceHandle h, const Vec3& pos, const Vec3& vel);
    void setListener(const Listener& l);
    void setWindowFocused(bool f) { focused_ = f; }

    // Occlusion: built-in AABB occluders and/or a ray callback from the engine
    // returning how much of the segment is blocked (0 = clear, 1 = blocked).
    void addOccluderBox(const Vec3& mn, const Vec3& mx);
    const std::vector<std::pair<Vec3, Vec3>>& occluders() const { return occluders_; }
    void setRayQuery(std::function<float(const Vec3&, const Vec3&)> fn) { rayQuery_ = std::move(fn); }

    void showSubtitle(const std::string& speaker, const std::string& text, float seconds, bool caption,
                      const Vec3* pos = nullptr);
    const std::vector<Subtitle>& subtitles() const { return subtitles_; }
    const std::vector<SoundEvent>& soundEvents() const { return events_; }

    // Game thread, once per frame.
    void update(float dt);
    AudioStats stats() const { return stats_; }

    // Audio device thread entry point (called from miniaudio's data callback).
    void onAudio(void* out, unsigned frames);
    const Listener& listener() const { return listener_; }

private:
    bool openDevice();
    void closeDevice();
    void restartStreams();
    VoiceHandle allocVoice(const PlayParams& p, int sound, int stream);
    float occlusionFor(const Vec3& from, const Vec3& to) const;

    AudioSettings settings_;
    std::string cacheDir_;
    void* context_ = nullptr;   // ma_context*
    void* device_ = nullptr;    // ma_device*
    bool deviceRunning_ = false;
    std::vector<std::string> deviceNames_;
    std::vector<std::array<unsigned char, 256>> deviceIds_;
    SampleFormatSetting deviceFormat_ = SampleFormatSetting::F32;

    std::vector<SoundAsset> assets_;
    std::vector<std::unique_ptr<StreamSource>> streams_;
    std::vector<std::string> streamPaths_;
    std::vector<StreamSource*> streamPtrs_;

    // Shared with the audio thread, guarded by mutex_ (audio thread only try_locks).
    mutable std::mutex mutex_;
    Mixer mixer_;
    std::array<VoiceDesc, kMaxVoiceRecords> descs_{};
    std::array<VoiceFeedback, kMaxVoiceRecords> feedback_{};
    MixerCounters counters_;
    Listener listener_;
    bool focused_ = true;

    // Game-thread state
    std::vector<std::pair<Vec3, Vec3>> occluders_;
    std::function<float(const Vec3&, const Vec3&)> rayQuery_;
    int occlusionCursor_ = 0;
    std::vector<Subtitle> subtitles_;
    std::vector<SoundEvent> events_;
    std::array<const char*, kMaxVoiceRecords> captions_{};

    // stats
    AudioStats stats_;
    MixerCounters lastCounters_;
    double lastStreamSec_ = 0.0, statsTimer_ = 0.0, statsWall_ = 0.0;
};

}  // namespace gfx::audio
