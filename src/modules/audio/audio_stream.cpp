#include "audio_stream.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../../../third_party/miniaudio/miniaudio.h"

namespace gfx::audio {

StreamSource::~StreamSource() { close(); }

bool StreamSource::open(const std::string& path, int bufferMs, bool threaded, bool loop) {
    close();
    auto* dec = new ma_decoder;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);  // keep native channels & rate
    if (ma_decoder_init_file(path.c_str(), &cfg, dec) != MA_SUCCESS) {
        delete dec;
        return false;
    }
    decoder_ = dec;
    channels_ = int(dec->outputChannels);
    sampleRate_ = dec->outputSampleRate;
    loop_ = loop;
    threaded_ = threaded;
    capacityFrames_ = std::max(256, int(int64_t(sampleRate_) * bufferMs / 1000));
    ring_.assign(size_t(capacityFrames_) * channels_, 0.0f);
    decodeScratch_.assign(size_t(std::max(capacityFrames_ / 4, 256)) * channels_, 0.0f);
    writePos_ = 0; readPos_ = 0; underruns_ = 0; decodeNs_ = 0;
    decodeInto(capacityFrames_);  // pre-fill before the first read (what a loading screen does)
    if (threaded_) {
        running_ = true;
        thread_ = std::thread(&StreamSource::threadMain, this);
    }
    return true;
}

void StreamSource::close() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (decoder_) {
        ma_decoder_uninit(static_cast<ma_decoder*>(decoder_));
        delete static_cast<ma_decoder*>(decoder_);
        decoder_ = nullptr;
    }
    ring_.clear(); ring_.shrink_to_fit();
    decodeScratch_.clear(); decodeScratch_.shrink_to_fit();
}

int StreamSource::bufferedFrames() const { return int(writePos_.load() - readPos_.load()); }

int StreamSource::decodeInto(int frames) {
    auto* dec = static_cast<ma_decoder*>(decoder_);
    const int chunkCap = int(decodeScratch_.size() / channels_);
    int pushed = 0;
    while (pushed < frames) {
        uint64_t w = writePos_.load(std::memory_order_relaxed);
        uint64_t r = readPos_.load(std::memory_order_acquire);
        int freeFrames = capacityFrames_ - int(w - r);
        int want = std::min({frames - pushed, freeFrames, chunkCap});
        if (want <= 0) break;

        auto t0 = std::chrono::steady_clock::now();
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(dec, decodeScratch_.data(), ma_uint64(want), &got);
        if (got == 0) {
            if (!loop_) break;
            ma_decoder_seek_to_pcm_frame(dec, 0);
            continue;
        }
        decodeNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());

        for (ma_uint64 i = 0; i < got; ++i) {
            size_t dst = size_t((w + i) % uint64_t(capacityFrames_)) * channels_;
            std::memcpy(&ring_[dst], &decodeScratch_[size_t(i) * channels_], sizeof(float) * channels_);
        }
        writePos_.store(w + got, std::memory_order_release);
        pushed += int(got);
    }
    return pushed;
}

void StreamSource::threadMain() {
    const int chunk = int(decodeScratch_.size() / channels_);
    uint32_t n = 0;
    while (running_) {
        int freeFrames = capacityFrames_ - bufferedFrames();
        if (freeFrames >= chunk) {
            if (slowDisk_) {
                // Pretend this is a busy HDD: 20 ms per read, and every 8th read
                // waits behind a 250 ms seek (another asset load, antivirus...).
                std::this_thread::sleep_for(std::chrono::milliseconds((++n % 8 == 0) ? 250 : 20));
            }
            decodeInto(chunk);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void StreamSource::pumpSync(int framesWanted) {
    if (!threaded_ && decoder_) decodeInto(std::max(0, framesWanted - bufferedFrames()));
}

int StreamSource::read(float* out, int frames) {
    if (!decoder_) {
        std::memset(out, 0, sizeof(float) * size_t(frames) * std::max(channels_, 1));
        return 0;
    }
    uint64_t r = readPos_.load(std::memory_order_relaxed);
    uint64_t w = writePos_.load(std::memory_order_acquire);
    int avail = int(w - r);
    int n = std::min(avail, frames);
    for (int i = 0; i < n; ++i) {
        size_t src = size_t((r + i) % uint64_t(capacityFrames_)) * channels_;
        std::memcpy(out + size_t(i) * channels_, &ring_[src], sizeof(float) * channels_);
    }
    if (n < frames) {
        std::memset(out + size_t(n) * channels_, 0, sizeof(float) * size_t(frames - n) * channels_);
        ++underruns_;
    }
    readPos_.store(r + n, std::memory_order_release);
    return n;
}

}  // namespace gfx::audio
