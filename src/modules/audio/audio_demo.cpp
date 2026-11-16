#include "audio_demo.h"

#include <cmath>

namespace gfx::audio {

namespace {
struct Line { const char* speaker; const char* text; };
const Line kLines[] = {
    {"Mara", "Hear that hum behind the wall? Walk around it and listen to it open up."},
    {"Iven", "Car coming through. Notice the pitch drop as it passes."},
    {"Mara", "Try night mode and the explosion stops shaking the windows."},
    {"Iven", "Switch to headphones and HRTF, then close your eyes."},
};
}  // namespace

float AudioDemo::rand01() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return float(rng_ >> 8) / 16777216.0f;
}

void AudioDemo::start(AudioSystem& a, const std::string& cacheDir, int crowd) {
    hum_ = a.addSound(synth::engineHum());
    car_ = a.addSound(synth::carEngine());
    explosion_ = a.addSound(synth::explosion());
    wind_ = a.addSound(synth::wind());
    click_ = a.addSound(synth::uiClick());
    for (uint32_t s = 1; s <= 4; ++s) footsteps_.push_back(a.addSound(synth::footstep(48000, s)));
    for (uint32_t s = 0; s < 4; ++s) lines_.push_back(a.addSound(synth::voiceLine(3.2f, s + 1)));

    // Music is written to disk once and streamed back, like a real soundtrack.
    const std::string musicPath = cacheDir + "/gfxlab_music_loop.wav";
    synth::writeWav(musicPath, synth::musicLoop());
    music_ = a.addStream(musicPath);

    a.addOccluderBox({kWallMinX, 0.0f, kWallZ - 0.5f}, {kWallMaxX, 4.0f, kWallZ + 0.5f});

    PlayParams p;
    if (music_ >= 0) {
        p.bus = Bus::Music; p.is3D = false; p.loop = true; p.priority = 255; p.reverbSend = 0.0f;
        a.playStream(music_, p);
    }
    p = {};
    p.bus = Bus::Ambient; p.is3D = false; p.loop = true; p.priority = 200; p.volume = 0.5f;
    a.play(wind_, p);

    p = {};
    p.loop = true; p.priority = 180; p.refDistance = 2.0f; p.maxDistance = 40.0f;
    orbit_ = a.play(hum_, p);

    // The crowd: many quiet looping machines scattered around. Only the loudest
    // `max voices` are mixed; the rest run as virtual voices.
    for (int i = 0; i < crowd; ++i) {
        p = {};
        p.loop = true; p.priority = 64; p.volume = 0.25f;
        p.pitch = 0.6f + 1.2f * rand01();
        float ang = rand01() * 6.2831853f, r = 6.0f + 50.0f * rand01();
        p.position = {std::cos(ang) * r, 0.5f, std::sin(ang) * r};
        p.refDistance = 1.0f; p.maxDistance = 50.0f;
        a.play(hum_, p);
    }
}

void AudioDemo::click(AudioSystem& a) {
    PlayParams p;
    p.bus = Bus::Ui; p.is3D = false; p.priority = 250; p.reverbSend = 0.0f;
    a.play(click_, p);
}

void AudioDemo::update(AudioSystem& a, float dt) {
    time_ += dt;

    // Orbiting hum: radius 8 around (0, 1, -14) so it slips behind the wall at z = -9.
    orbitAngle_ += dt * 0.35f;
    const Vec3 c{0.0f, 1.0f, -14.0f};
    const Vec3 pos{c.x + std::cos(orbitAngle_) * 8.0f, c.y, c.z + std::sin(orbitAngle_) * 8.0f};
    const Vec3 vel{-std::sin(orbitAngle_) * 8.0f * 0.35f, 0.0f, std::cos(orbitAngle_) * 8.0f * 0.35f};
    a.setVoicePosition(orbit_, pos, vel);

    // Car fly-by along x at 30 m/s, 8 m in front of the origin.
    carTimer_ -= dt;
    if (carTimer_ <= 0.0f && !a.isPlaying(carVoice_)) {
        carX_ = -90.0f;
        PlayParams p;
        p.loop = true; p.priority = 220; p.refDistance = 4.0f; p.maxDistance = 120.0f;
        p.position = {carX_, 0.5f, -2.0f};
        p.caption = "[Car passing]";
        carVoice_ = a.play(car_, p);
        carTimer_ = 9.0f;
    }
    if (a.isPlaying(carVoice_)) {
        carX_ += 30.0f * dt;
        a.setVoicePosition(carVoice_, {carX_, 0.5f, -2.0f}, {30.0f, 0.0f, 0.0f});
        if (carX_ > 90.0f) a.stop(carVoice_);
    }

    // Explosions far away: big transient for the dynamic-range setting.
    boomTimer_ -= dt;
    if (boomTimer_ <= 0.0f) {
        PlayParams p;
        float ang = rand01() * 6.2831853f;
        p.position = {std::cos(ang) * 30.0f, 2.0f, std::sin(ang) * 30.0f};
        p.priority = 240; p.refDistance = 10.0f; p.maxDistance = 200.0f; p.volume = 1.0f;
        p.caption = "[Explosion]";
        a.play(explosion_, p);
        boomTimer_ = 14.0f;
    }

    // Footsteps walking around the listener.
    stepTimer_ -= dt;
    if (stepTimer_ <= 0.0f) {
        PlayParams p;
        const Vec3 l = a.listener().position;
        float ang = time_ * 0.5f;
        p.position = {l.x + std::cos(ang) * 3.0f, 0.0f, l.z + std::sin(ang) * 3.0f};
        p.priority = 150; p.volume = 0.7f;
        a.play(footsteps_[size_t(int(rand01() * 4) % 4)], p);
        stepTimer_ = 0.5f;
    }

    // Dialogue from an NPC standing to the front-left, with subtitles.
    lineTimer_ -= dt;
    if (lineTimer_ <= 0.0f) {
        const Line& ln = kLines[lineIndex_ % 4];
        PlayParams p;
        p.bus = Bus::Voice; p.priority = 245; p.position = {-3.0f, 1.7f, -3.0f}; p.refDistance = 3.0f; p.maxDistance = 40.0f;
        a.playVoiceLine(lines_[size_t(lineIndex_ % 4)], p, ln.speaker, ln.text, 3.5f);
        ++lineIndex_;
        lineTimer_ = 10.0f;
    }
}

}  // namespace gfx::audio
