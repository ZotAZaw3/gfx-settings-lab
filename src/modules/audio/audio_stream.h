// Disk streaming source: a decoder thread keeps a ring buffer of decoded PCM
// topped up, the audio thread drains it. See docs/audio_streaming_buffer.md.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace gfx::audio {

class StreamSource {
public:
    StreamSource() = default;
    ~StreamSource();
    StreamSource(const StreamSource&) = delete;
    StreamSource& operator=(const StreamSource&) = delete;

    // Opens `path` (wav/flac/mp3 via miniaudio's decoder). `bufferMs` sizes the
    // ring buffer. With `threaded == false` the caller pumps decoding itself
    // (used by the offline benchmark, which renders faster than real time).
    bool open(const std::string& path, int bufferMs, bool threaded, bool loop = true);
    void close();

    // Audio thread: pops up to `frames` frames of interleaved float. Missing
    // frames are zero-filled and counted as an underrun.
    int read(float* out, int frames);
    void pumpSync(int framesWanted);  // non-threaded mode only

    void setSimulateSlowDisk(bool v) { slowDisk_ = v; }
    int channels() const { return channels_; }
    uint32_t sampleRate() const { return sampleRate_; }
    uint64_t underruns() const { return underruns_.load(); }
    double decodeSeconds() const { return decodeNs_.load() * 1e-9; }  // time spent decoding
    size_t memoryBytes() const { return ring_.capacity() * sizeof(float) + decodeScratch_.capacity() * sizeof(float); }
    int bufferedFrames() const;
    int capacityFrames() const { return capacityFrames_; }
    bool isOpen() const { return decoder_ != nullptr; }

private:
    void threadMain();
    int decodeInto(int frames);  // decodes and pushes, returns frames pushed

    void* decoder_ = nullptr;  // ma_decoder*
    std::vector<float> ring_;
    std::vector<float> decodeScratch_;
    int capacityFrames_ = 0, channels_ = 0;
    uint32_t sampleRate_ = 0;
    bool loop_ = true, threaded_ = true;
    std::atomic<uint64_t> writePos_{0}, readPos_{0};  // in frames, monotonically increasing
    std::atomic<uint64_t> underruns_{0}, decodeNs_{0};
    std::atomic<bool> running_{false}, slowDisk_{false};
    std::thread thread_;
};

}  // namespace gfx::audio
