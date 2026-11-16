// Procedurally generated test sounds, so the prototype ships with zero audio
// assets and every run is reproducible.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gfx::audio {

struct SoundAsset {
    std::string name;
    uint32_t sampleRate = 48000;
    int channels = 1;
    std::vector<float> data;  // interleaved float, -1..1
    size_t frames() const { return channels ? data.size() / channels : 0; }
    size_t memoryBytes() const { return data.capacity() * sizeof(float); }
};

namespace synth {
SoundAsset engineHum(uint32_t sr = 48000);     // 2 s loop, 3D source
SoundAsset carEngine(uint32_t sr = 48000);     // 1 s loop, fast-moving source (Doppler)
SoundAsset footstep(uint32_t sr = 48000, uint32_t seed = 1);
SoundAsset explosion(uint32_t sr = 48000);
SoundAsset wind(uint32_t sr = 48000);          // 4 s loop, ambience
SoundAsset uiClick(uint32_t sr = 48000);
SoundAsset voiceLine(float seconds, uint32_t seed, uint32_t sr = 48000);  // formant "babble"
SoundAsset musicLoop(uint32_t sr = 44100);     // 24 s stereo; written to disk and streamed

bool writeWav(const std::string& path, const SoundAsset& a);
}  // namespace synth

}  // namespace gfx::audio
