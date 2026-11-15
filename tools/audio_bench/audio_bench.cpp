// Offline audio benchmark: renders the demo soundscape through gfx::audio::Mixer
// for every value of every audio setting and reports CPU (% of one core on the
// audio thread) and RAM. No audio device needed, so it runs in CI.
//
//   audio_bench [--seconds 8] [--out docs/_measurements/audio.md] [--wav demo.wav]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio_mixer.h"
#include "audio_settings.h"
#include "audio_stream.h"
#include "audio_synth.h"
#include "audio_system.h"

using namespace gfx::audio;
using Clock = std::chrono::steady_clock;

struct Scene {
    std::vector<SoundAsset> assets;
    std::string musicPath;
    std::vector<std::pair<Vec3, Vec3>> boxes;
};

struct Result {
    std::string group, value;
    double cpuPct = 0, stagePct[kStageCount]{};
    double ramKB = 0, latencyMs = 0, occlusionUs = 0;
    int real = 0, virt = 0;
};

static double gSeconds = 8.0;

static Result run(const Scene& scene, const AudioSettings& s, int crowd, bool focused = true) {
    const uint32_t sr = sampleRateHz(s.sampleRate) ? sampleRateHz(s.sampleRate) : 48000;
    const int ch = speakerChannels(s.speakers) ? speakerChannels(s.speakers) : 2;

    StreamSource music;
    music.open(scene.musicPath, s.streamBufferMs, /*threaded*/ false);
    std::vector<StreamSource*> streams{&music};

    Mixer mixer;
    mixer.setAssets(&scene.assets);
    mixer.setStreams(&streams);
    mixer.configure(sr, ch, s);

    std::array<VoiceDesc, kMaxVoiceRecords> descs{};
    std::array<VoiceFeedback, kMaxVoiceRecords> fb{};
    MixerCounters counters;
    int n = 0;
    auto add = [&](int sound, int stream, Bus bus, bool is3D, Vec3 pos, float vol, float pitch, int prio, float ref, float mx) {
        VoiceDesc& d = descs[size_t(n++)];
        d.generation = 1; d.active = true; d.sound = sound; d.stream = stream; d.bus = bus; d.is3D = is3D; d.loop = true;
        d.position = pos; d.volume = vol; d.pitch = pitch; d.priority = prio; d.refDistance = ref; d.maxDistance = mx;
        d.reverbSend = (bus == Bus::Music) ? 0.0f : 1.0f;
    };
    add(-1, 0, Bus::Music, false, {}, 1.0f, 1.0f, 255, 1, 1);           // streamed music
    add(3, -1, Bus::Ambient, false, {}, 0.5f, 1.0f, 200, 1, 1);         // wind
    add(0, -1, Bus::Sfx, true, {8, 1, -14}, 1.0f, 1.0f, 180, 2, 40);    // orbiting hum
    add(1, -1, Bus::Sfx, true, {-40, 0.5f, -2}, 1.0f, 1.0f, 220, 4, 120);  // car (moving)
    add(2, -1, Bus::Sfx, true, {20, 2, 20}, 1.0f, 1.0f, 240, 10, 200);  // explosion (looped for steady load)
    uint32_t rng = 1234;
    auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return float(rng >> 8) / 16777216.0f; };
    for (int i = 0; i < crowd && n < kMaxVoiceRecords; ++i) {
        float a = rnd() * 6.2831853f, r = 6.0f + 50.0f * rnd();
        add(0, -1, Bus::Sfx, true, {std::cos(a) * r, 0.5f, std::sin(a) * r}, 0.25f, 0.6f + 1.2f * rnd(), 64, 1, 50);
    }

    // Occlusion is game-thread work: time the ray tests for every 3D voice once.
    Result res;
    {
        Listener l;
        auto t0 = Clock::now();
        int tests = 0;
        for (int rep = 0; rep < 20; ++rep)
            for (int i = 0; i < n; ++i)
                if (descs[size_t(i)].is3D) {
                    descs[size_t(i)].occlusion = computeOcclusion(s.occlusion, l.position, descs[size_t(i)].position, scene.boxes);
                    ++tests;
                }
        res.occlusionUs = std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / 20.0;
        (void)tests;
    }

    Listener listener;
    listener.velocity = {0, 0, 0};
    const int block = std::max(32, s.bufferFrames);
    const int total = int(gSeconds * sr);
    std::vector<unsigned char> out(size_t(block) * ch * 4);
    float carX = -40.0f;
    // warm-up (fills reverb tails, voice selection settles)
    mixer.sync(s, listener, focused, descs, fb, counters);
    mixer.renderToFormat(out.data(), s.format, block);
    MixerCounters before = mixer.counters();
    auto wall0 = Clock::now();
    for (int done = 0; done < total; done += block) {
        carX += 30.0f * float(block) / sr;
        if (carX > 40.0f) carX = -40.0f;
        descs[3].position = {carX, 0.5f, -2.0f};
        descs[3].velocity = {30.0f, 0.0f, 0.0f};
        auto c0 = Clock::now();
        mixer.sync(s, listener, focused, descs, fb, counters);
        if (!(s.background == BackgroundAudio::Pause && !focused && mixer.backgroundFadeDone()))
            mixer.renderToFormat(out.data(), s.format, std::min(block, total - done));
        mixer.addCallbackNs(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - c0).count()));
    }
    const double wallSec = std::chrono::duration<double>(Clock::now() - wall0).count();
    (void)wallSec;
    const MixerCounters& after = mixer.counters();
    const double audioSec = double(total) / sr;
    res.cpuPct = double(after.callbackNs - before.callbackNs) / (audioSec * 1e9) * 100.0;
    for (int st = 0; st < kStageCount; ++st)
        res.stagePct[st] = double(after.stageNs[size_t(st)] - before.stageNs[size_t(st)]) / (audioSec * 1e9) * 100.0;
    size_t mem = music.memoryBytes() + size_t(block) * s.periods * ch * size_t(bytesPerSample(s.format));
    for (size_t m : after.memBytes) mem += m;
    res.ramKB = double(mem) / 1024.0;
    res.latencyMs = 1000.0 * block * s.periods / sr;
    res.real = after.realVoices;
    res.virt = after.virtualVoices;
    return res;
}

int main(int argc, char** argv) {
    std::string outPath, wavPath;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) gSeconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wavPath = argv[++i];
    }

    Scene scene;
    scene.assets.push_back(synth::engineHum());
    scene.assets.push_back(synth::carEngine());
    scene.assets.push_back(synth::explosion());
    scene.assets.push_back(synth::wind());
    scene.musicPath = "audio_bench_music.wav";
    synth::writeWav(scene.musicPath, synth::musicLoop());
    scene.boxes.push_back({{-3, 0, -9.5f}, {3, 4, -8.5f}});
    size_t assetBytes = 0;
    for (auto& a : scene.assets) assetBytes += a.memoryBytes();

    AudioSettings base;
    applyQualityPreset(base, QualityPreset::High);
    base.speakers = SpeakerConfig::Stereo;
    const int kCrowd = 64;

    std::vector<Result> results;
    auto sweep = [&](const char* group, int count, const std::function<std::string(AudioSettings&, int&, int)>& apply) {
        for (int v = 0; v < count; ++v) {
            AudioSettings s = base;
            int crowd = kCrowd;
            std::string label = apply(s, crowd, v);
            Result r = run(scene, s, crowd);
            r.group = group;
            r.value = label;
            results.push_back(r);
            std::fprintf(stderr, "%-22s %-28s cpu %6.2f%%  ram %8.1f KB\n", group, label.c_str(), r.cpuPct, r.ramKB);
        }
    };

    sweep("Baseline (High)", 1, [](AudioSettings&, int&, int) { return std::string("High preset, stereo 48 kHz f32"); });
    sweep("Quality preset", 4, [](AudioSettings& s, int&, int v) { applyQualityPreset(s, QualityPreset(v)); return std::string(toString(QualityPreset(v))); });
    sweep("Max voices", 7, [](AudioSettings& s, int& crowd, int v) {
        static const int k[] = {16, 32, 64, 96, 128, 192, 256};
        s.maxVoices = k[v]; crowd = 300;
        return std::to_string(k[v]) + " (305 playing)";
    });
    sweep("Resampler", 4, [](AudioSettings& s, int&, int v) { s.resampler = ResamplerQuality(v); return std::string(toString(ResamplerQuality(v))); });
    sweep("Spatial audio", 3, [](AudioSettings& s, int&, int v) { s.spatial = SpatialMode(v); return std::string(toString(SpatialMode(v))); });
    sweep("Speaker config", 5, [](AudioSettings& s, int&, int v) {
        static const SpeakerConfig k[] = {SpeakerConfig::Mono, SpeakerConfig::Stereo, SpeakerConfig::Quad, SpeakerConfig::Surround51, SpeakerConfig::Surround71};
        s.speakers = k[v]; return std::string(toString(k[v]));
    });
    sweep("Sample rate", 4, [](AudioSettings& s, int&, int v) { s.sampleRate = SampleRateSetting(v + 1); return std::string(toString(SampleRateSetting(v + 1))); });
    sweep("Bit depth", 4, [](AudioSettings& s, int&, int v) {
        static const SampleFormatSetting k[] = {SampleFormatSetting::S16, SampleFormatSetting::S16, SampleFormatSetting::S24, SampleFormatSetting::F32};
        s.format = k[v]; s.dither = v != 1;
        return std::string(toString(k[v])) + (k[v] != SampleFormatSetting::F32 ? (s.dither ? " + dither" : ", no dither") : "");
    });
    sweep("Reverb", 3, [](AudioSettings& s, int&, int v) { s.reverb = ReverbQuality(v); return std::string(toString(ReverbQuality(v))); });
    sweep("Occlusion", 3, [](AudioSettings& s, int&, int v) { s.occlusion = OcclusionMode(v); return std::string(toString(OcclusionMode(v))); });
    sweep("Distance attenuation", 4, [](AudioSettings& s, int&, int v) { s.attenuation = AttenuationModel(v); return std::string(toString(AttenuationModel(v))); });
    sweep("Air absorption", 2, [](AudioSettings& s, int&, int v) { s.airAbsorption = v; return std::string(v ? "On" : "Off"); });
    sweep("Doppler", 2, [](AudioSettings& s, int&, int v) { s.doppler = v; return std::string(v ? "On" : "Off"); });
    sweep("Dynamic range", 3, [](AudioSettings& s, int&, int v) { s.dynamicRange = DynamicRange(v); return std::string(toString(DynamicRange(v))); });
    sweep("Buffer size", 7, [](AudioSettings& s, int&, int v) {
        static const int k[] = {64, 128, 256, 480, 1024, 2048, 4096};
        s.bufferFrames = k[v]; return std::to_string(k[v]) + " frames x 2";
    });
    sweep("Streaming buffer", 5, [](AudioSettings& s, int&, int v) {
        static const int k[] = {100, 250, 500, 1000, 4000};
        s.streamBufferMs = k[v]; return std::to_string(k[v]) + " ms";
    });
    sweep("Mono audio", 2, [](AudioSettings& s, int&, int v) { s.monoAudio = v; return std::string(v ? "On" : "Off"); });

    // Background behaviour (unfocused window)
    for (int v = 0; v < 4; ++v) {
        AudioSettings s = base;
        s.background = BackgroundAudio(v);
        Result r = run(scene, s, kCrowd, /*focused*/ false);
        r.group = "Background (unfocused)";
        r.value = toString(BackgroundAudio(v));
        results.push_back(r);
        std::fprintf(stderr, "%-22s %-28s cpu %6.2f%%\n", r.group.c_str(), r.value.c_str(), r.cpuPct);
    }

    // Streaming underruns: real time, threaded decoder, simulated slow disk.
    struct Under { int ms; uint64_t underruns; };
    std::vector<Under> under;
    for (int ms : {100, 250, 500, 1000}) {
        StreamSource st;
        st.open(scene.musicPath, ms, true);
        st.setSimulateSlowDisk(true);
        std::vector<float> buf(480 * 2);
        auto t0 = Clock::now();
        for (int i = 0; i < 300; ++i) {  // 3 s of real time, 10 ms reads
            st.read(buf.data(), 441);
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(10 * (i + 1)));
        }
        under.push_back({ms, st.underruns()});
        std::fprintf(stderr, "slow disk, %4d ms buffer: %llu underruns in 3 s\n", ms, (unsigned long long)st.underruns());
    }

    // ---- report ----
    std::ostringstream md;
    unsigned hc = std::thread::hardware_concurrency();
    md << "# Audio settings: measured cost\n\n"
       << "Generated by `tools/audio_bench` (offline render of the demo soundscape, " << gSeconds
       << " s per row). CPU is the audio thread's busy time as a **percentage of one core** "
       << "(time spent mixing / duration of audio produced). RAM is the memory the mixer, streams and device "
       << "buffer allocate for that setting; decoded sound assets add a constant " << (assetBytes / 1024) << " KB. "
       << "GPU cost of every audio setting is 0: audio never touches the GPU.\n\n"
       << "Machine: cloud container, " << hc << " vCPU, compiler optimisation -O2. Absolute numbers are "
       << "slower than a desktop CPU; compare rows with each other.\n\n"
       << "Baseline: High preset (96 voices, cubic resampler, high reverb, multi-ray occlusion), stereo, 48 kHz, "
       << "32-bit float, 480-frame buffer, 69 playing voices (5 scene sounds + 64 crowd emitters).\n\n";
    std::string group;
    for (auto& r : results) {
        if (r.group != group) {
            group = r.group;
            md << "\n## " << group << "\n\n| Value | Audio CPU (% of 1 core) | Own stage CPU | RAM (KB) | Real / virtual voices | Buffer latency (ms) | Occlusion rays (µs / frame, game thread) | Top stages |\n|---|---|---|---|---|---|---|---|\n";
        }
        int idx[kStageCount];
        for (int i = 0; i < kStageCount; ++i) idx[i] = i;
        std::sort(idx, idx + kStageCount, [&](int a, int b) { return r.stagePct[a] > r.stagePct[b]; });
        std::string own = "-";
        static const std::pair<const char*, Stage> kOwn[] = {
            {"Resampler", Stage::Resample}, {"Spatial audio", Stage::Spatialize}, {"Reverb", Stage::Reverb},
            {"Occlusion", Stage::Filter}, {"Air absorption", Stage::Filter}, {"Dynamic range", Stage::Dynamics},
            {"Speaker config", Stage::BassManagement}, {"Bit depth", Stage::Output}, {"Mono audio", Stage::Master},
            {"Doppler", Stage::Spatialize}, {"Distance attenuation", Stage::Spatialize}, {"Buffer size", Stage::VoiceSelect}};
        for (auto& o : kOwn)
            if (r.group == o.first) {
                char ob[96];
                std::snprintf(ob, sizeof(ob), "%s %.3f", toString(o.second), r.stagePct[int(o.second)]);
                own = ob;
            }
        char buf[768];
        std::snprintf(buf, sizeof(buf), "| %s | %.2f | %s | %.0f | %d / %d | %.1f | %.1f | %s %.2f, %s %.2f |\n", r.value.c_str(), r.cpuPct, own.c_str(), r.ramKB,
                      r.real, r.virt, r.latencyMs, r.occlusionUs, toString(Stage(idx[0])), r.stagePct[idx[0]],
                      toString(Stage(idx[1])), r.stagePct[idx[1]]);
        md << buf;
    }
    md << "\n## Streaming underruns (simulated slow HDD, 3 s real time)\n\n| Stream buffer | Underruns |\n|---|---|\n";
    for (auto& u : under) md << "| " << u.ms << " ms | " << u.underruns << " |\n";

    std::string text = md.str();
    if (!outPath.empty()) {
        std::ofstream(outPath) << text;
        std::fprintf(stderr, "wrote %s\n", outPath.c_str());
    } else {
        std::fputs(text.c_str(), stdout);
    }

    if (!wavPath.empty()) {  // listenable 10 s render of the baseline mix
        StreamSource music;
        music.open(scene.musicPath, 500, false);
        std::vector<StreamSource*> streams{&music};
        Mixer m;
        m.setAssets(&scene.assets);
        m.setStreams(&streams);
        m.configure(48000, 2, base);
        std::array<VoiceDesc, kMaxVoiceRecords> descs{};
        std::array<VoiceFeedback, kMaxVoiceRecords> fb{};
        MixerCounters c;
        descs[0].generation = 1; descs[0].active = true; descs[0].stream = 0; descs[0].bus = Bus::Music; descs[0].is3D = false; descs[0].reverbSend = 0;
        descs[1].generation = 1; descs[1].active = true; descs[1].sound = 1; descs[1].loop = true; descs[1].refDistance = 4; descs[1].maxDistance = 120;
        SoundAsset outA{"mix", 48000, 2, {}};
        std::vector<float> blk(480 * 2);
        for (int i = 0; i < 1000; ++i) {
            float x = -60.0f + 120.0f * i / 1000.0f;
            descs[1].position = {x, 0.5f, -3.0f};
            descs[1].velocity = {30.0f, 0, 0};
            m.sync(base, Listener{}, true, descs, fb, c);
            m.render(blk.data(), 480);
            outA.data.insert(outA.data.end(), blk.begin(), blk.end());
        }
        synth::writeWav(wavPath, outA);
        std::fprintf(stderr, "wrote %s\n", wavPath.c_str());
    }
    return 0;
}
