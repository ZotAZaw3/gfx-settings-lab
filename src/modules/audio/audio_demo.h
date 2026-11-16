// Demo soundscape that exercises every audio setting: streamed music, ambience,
// an orbiting 3D source that passes behind a wall (occlusion), a car fly-by
// (Doppler), a crowd of quiet emitters (voice limit / virtual voices),
// explosions (dynamic range, captions) and NPC dialogue (subtitles).
#pragma once

#include <string>
#include <vector>

#include "audio_system.h"

namespace gfx::audio {

class AudioDemo {
public:
    void start(AudioSystem& a, const std::string& cacheDir, int crowd = 64);
    void update(AudioSystem& a, float dt);
    void click(AudioSystem& a);   // UI sound, played when a setting changes

    static constexpr float kWallMinX = -3.0f, kWallMaxX = 3.0f, kWallZ = -9.0f;

private:
    int hum_ = -1, car_ = -1, explosion_ = -1, wind_ = -1, click_ = -1, music_ = -1;
    std::vector<int> footsteps_, lines_;
    VoiceHandle orbit_, carVoice_;
    float time_ = 0.0f, orbitAngle_ = 0.0f, carTimer_ = 2.0f, carX_ = 0.0f;
    float boomTimer_ = 6.0f, lineTimer_ = 3.0f, stepTimer_ = 0.0f;
    int lineIndex_ = 0;
    unsigned rng_ = 12345u;
    float rand01();
};

}  // namespace gfx::audio
