// gfx::Module wrapper: registers the "Audio" settings tab, drives AudioSystem
// from the camera every frame, draws subtitles / sound visualization, and
// reports audio CPU / RAM to the profiler.
#include <cmath>
#include <filesystem>

#include "core/engine.h"
#include "core/profiler.h"
#include "core/renderer.h"
#include "core/settings.h"
#include "imgui.h"

#include "audio_demo.h"
#include "audio_system.h"

namespace gfx {
namespace {

using namespace gfx::audio;

const int kBufferFrames[] = {64, 128, 256, 480, 512, 1024, 2048, 4096};

class AudioModule final : public Module {
public:
    const char* name() const override { return "audio"; }
    int priority() const override { return 50; }

    void registerSettings(SettingsRegistry& r) override {
        const std::string cat = "Audio";
        auto desc = [&](const char* id, const char* label, const char* group, const char* tip, const char* doc, unsigned flags = 0) {
            return SettingDesc{id, label, cat, group, tip, doc, flags};
        };
        auto names = [](auto count, auto fn) {
            std::vector<std::string> v;
            for (int i = 0; i < int(count); ++i) v.emplace_back(fn(i));
            return v;
        };

        // Volume
        master_ = &r.addFloat(desc("audio.volume.master", "Master volume", "Volume", "Overall loudness of the game, applied last.", "audio_volume_mixing", SettingNoPreset), 80, 0, 100).fmt("%.0f%%");
        static const char* busIds[] = {"audio.volume.music", "audio.volume.sfx", "audio.volume.voice", "audio.volume.ambient", "audio.volume.ui"};
        static const float busDef[] = {60, 100, 100, 70, 60};
        for (int b = 0; b < kBusCount; ++b)
            bus_[b] = &r.addFloat(desc(busIds[b], toString(Bus(b)), "Volume", "Volume of this mixer bus (category of sounds).", "audio_volume_mixing", SettingNoPreset), busDef[b], 0, 100).fmt("%.0f%%");
        mute_ = &r.addBool(desc("audio.volume.mute", "Mute all", "Volume", "Silence everything without losing your volume levels.", "audio_volume_mixing", SettingNoPreset), false);

        // Output
        std::vector<std::string> devs{"System default"};
        for (auto& n : enumerateOutputDevices()) devs.push_back(n);
        device_ = &r.addEnum(desc("audio.output.device", "Output device", "Output", "Which sound card / headset / HDMI output plays the game.", "audio_output_device", SettingNoPreset), devs, 0);
        speakers_ = &r.addEnum(desc("audio.output.speakers", "Speaker configuration", "Output", "Channel layout the mixer renders: stereo, headphones, 5.1, 7.1...", "audio_speaker_config", SettingNoPreset),
                               names(SpeakerConfig::Count, [](int i) { return toString(SpeakerConfig(i)); }), 0);
        lfe_ = &r.addFloat(desc("audio.output.lfe_crossover", "LFE crossover", "Output", "Bass below this frequency is also sent to the subwoofer.", "audio_speaker_config", SettingNoPreset), 80, 40, 200).fmt("%.0f Hz");
        lfe_->visible([this] { int v = speakers_->asInt(); return v == int(SpeakerConfig::Surround51) || v == int(SpeakerConfig::Surround71); });
        rate_ = &r.addEnum(desc("audio.output.sample_rate", "Sample rate", "Output", "Samples per second sent to the device.", "audio_sample_rate_bit_depth", SettingNoPreset),
                           names(SampleRateSetting::Count, [](int i) { return toString(SampleRateSetting(i)); }), int(SampleRateSetting::Hz48000));
        format_ = &r.addEnum(desc("audio.output.bit_depth", "Bit depth", "Output", "Sample format sent to the device.", "audio_sample_rate_bit_depth", SettingNoPreset),
                             names(SampleFormatSetting::Count, [](int i) { return toString(SampleFormatSetting(i)); }), int(SampleFormatSetting::F32));
        dither_ = &r.addBool(desc("audio.output.dither", "Dither", "Output", "Adds tiny noise when reducing to 16/24-bit so quiet fades don't turn grainy.", "audio_sample_rate_bit_depth", SettingNoPreset), true);
        bufferFrames_ = &r.addEnum(desc("audio.output.buffer", "Audio buffer size", "Latency", "Smaller = lower audio latency but more CPU and risk of crackles.", "audio_buffer_latency", SettingNoPreset),
                                   {"64", "128", "256", "480 (10 ms)", "512", "1024", "2048", "4096"}, 3);
        periods_ = &r.addInt(desc("audio.output.periods", "Buffer count", "Latency", "How many buffers are queued at the device (2 = lowest latency).", "audio_buffer_latency", SettingNoPreset), 2, 2, 4);

        // Quality (follow the global presets)
        voices_ = &r.addInt(desc("audio.quality.voices", "Max voices", "Quality", "How many sounds are actually mixed; the rest become silent 'virtual' voices.", "audio_quality_voices"), 96, 8, kMaxRealVoices);
        voices_->presets(24, 48, 96, 192);
        resampler_ = &r.addEnum(desc("audio.quality.resampler", "Resampling quality", "Quality", "Interpolation used when a sound's rate or pitch differs from the output.", "audio_resampling"),
                                names(ResamplerQuality::Count, [](int i) { return toString(ResamplerQuality(i)); }), int(ResamplerQuality::Cubic));
        resampler_->presets(1, 2, 2, 3);

        // 3D audio
        spatial_ = &r.addEnum(desc("audio.3d.spatial", "3D audio", "3D Audio", "How direction is rendered: pan, speaker panning, or HRTF for headphones.", "audio_spatial_hrtf"),
                              names(SpatialMode::Count, [](int i) { return toString(SpatialMode(i)); }), int(SpatialMode::SpeakerPanning));
        atten_ = &r.addEnum(desc("audio.3d.attenuation", "Distance attenuation", "3D Audio", "How fast sounds get quieter with distance.", "audio_distance_attenuation"),
                            names(AttenuationModel::Count, [](int i) { return toString(AttenuationModel(i)); }), int(AttenuationModel::Inverse));
        rolloff_ = &r.addFloat(desc("audio.3d.rolloff", "Rolloff factor", "3D Audio", "Multiplier on the attenuation curve.", "audio_distance_attenuation"), 1.0f, 0.1f, 4.0f);
        air_ = &r.addBool(desc("audio.3d.air_absorption", "Air absorption", "3D Audio", "Distant sounds lose treble, like in real air.", "audio_distance_attenuation"), true);
        air_->presets(0, 1, 1, 1);
        doppler_ = &r.addBool(desc("audio.3d.doppler", "Doppler effect", "3D Audio", "Pitch shift of moving sounds (the 'neeeoww' of a passing car).", "audio_doppler"), true);
        dopplerFactor_ = &r.addFloat(desc("audio.3d.doppler_factor", "Doppler strength", "3D Audio", "1 = physically correct; games often use 0.3-0.7.", "audio_doppler"), 1.0f, 0.0f, 3.0f);
        occlusion_ = &r.addEnum(desc("audio.3d.occlusion", "Sound occlusion", "3D Audio", "Walls between you and a sound muffle it (ray tests against the scene).", "audio_occlusion"),
                                names(OcclusionMode::Count, [](int i) { return toString(OcclusionMode(i)); }), int(OcclusionMode::MultiRay));
        occlusion_->presets(1, 1, 2, 2);

        // Environment
        reverb_ = &r.addEnum(desc("audio.env.reverb", "Reverb quality", "Environment", "Room echo / reflections simulation.", "audio_reverb"),
                             names(ReverbQuality::Count, [](int i) { return toString(ReverbQuality(i)); }), int(ReverbQuality::High));
        reverb_->presets(1, 1, 2, 2);
        wet_ = &r.addFloat(desc("audio.env.reverb_level", "Reverb level", "Environment", "How loud the room reflections are.", "audio_reverb"), 0.35f, 0.0f, 1.0f);
        room_ = &r.addFloat(desc("audio.env.room_size", "Room size", "Environment", "Longer decay for bigger rooms.", "audio_reverb"), 0.75f, 0.0f, 1.0f);
        damp_ = &r.addFloat(desc("audio.env.damping", "Damping", "Environment", "Soft surfaces absorb treble faster.", "audio_reverb"), 0.5f, 0.0f, 1.0f);

        // Dynamics
        dyn_ = &r.addEnum(desc("audio.mix.dynamic_range", "Dynamic range", "Dynamics", "Wide for home theater, narrow (night mode) to keep explosions and whispers closer in volume.", "audio_dynamic_range", SettingNoPreset),
                          names(DynamicRange::Count, [](int i) { return toString(DynamicRange(i)); }), 0);

        // Streaming
        stream_ = &r.addInt(desc("audio.stream.buffer_ms", "Streaming buffer", "Streaming", "How much music/dialogue is decoded ahead from disk.", "audio_streaming_buffer"), 500, 50, 4000);
        stream_->presets(250, 500, 500, 1000);
        slowDisk_ = &r.addBool(desc("audio.stream.slow_disk", "Simulate slow disk", "Streaming", "Debug: stalls the decoder like a busy HDD to show underruns.", "audio_streaming_buffer", SettingDebug | SettingNoPreset), false);

        // Background
        background_ = &r.addEnum(desc("audio.background.mode", "Audio in background", "Background", "What happens to sound when the game window loses focus.", "audio_background", SettingNoPreset),
                                 names(BackgroundAudio::Count, [](int i) { return toString(BackgroundAudio(i)); }), int(BackgroundAudio::Mute));
        duck_ = &r.addFloat(desc("audio.background.duck", "Background volume", "Background", "Volume used by 'Duck volume'.", "audio_background", SettingNoPreset), 20, 0, 100).fmt("%.0f%%");
        duck_->visible([this] { return background_->asInt() == int(BackgroundAudio::Duck); });

        // Accessibility
        mono_ = &r.addBool(desc("audio.access.mono", "Mono audio", "Accessibility", "Sum to mono, for single-sided hearing or one earbud.", "audio_accessibility", SettingNoPreset), false);
        balance_ = &r.addFloat(desc("audio.access.balance", "Left / right balance", "Accessibility", "Shift the mix toward one side.", "audio_accessibility", SettingNoPreset), 0.0f, -1.0f, 1.0f);
        viz_ = &r.addBool(desc("audio.access.visualize", "Visualize sound effects", "Accessibility", "On-screen indicators show where important sounds come from.", "audio_accessibility", SettingNoPreset), false);
        subs_ = &r.addBool(desc("audio.subtitles.enabled", "Subtitles", "Subtitles", "Show spoken dialogue as text.", "audio_subtitles", SettingNoPreset), true);
        captions_ = &r.addBool(desc("audio.subtitles.captions", "Closed captions", "Subtitles", "Also describe important sounds, e.g. [Explosion].", "audio_subtitles", SettingNoPreset), false);
        names_ = &r.addBool(desc("audio.subtitles.speaker_names", "Speaker names", "Subtitles", "Prefix lines with who is talking.", "audio_subtitles", SettingNoPreset), true);
        subSize_ = &r.addFloat(desc("audio.subtitles.size", "Subtitle size", "Subtitles", "Text scale.", "audio_subtitles", SettingNoPreset), 1.0f, 0.75f, 2.5f).fmt("%.2fx");
        subBg_ = &r.addFloat(desc("audio.subtitles.background", "Subtitle background", "Subtitles", "Opacity of the box behind the text.", "audio_subtitles", SettingNoPreset), 0.6f, 0.0f, 1.0f);

        stats_ = &r.addBool(desc("audio.debug.stats", "Audio stats window", "Debug", "Per-stage audio CPU, memory, meters and voices.", "audio_overview", SettingDebug | SettingNoPreset), false);
    }

    void init(Engine&) override {
        std::error_code ec;
        std::string cache = std::filesystem::temp_directory_path(ec).string();
        if (ec || cache.empty()) cache = ".";
        last_ = read();
        ok_ = sys_.init(last_, cache);
        if (ok_) demo_.start(sys_, cache);
    }

    void shutdown(Engine&) override { sys_.shutdown(); }

    void update(FrameContext& ctx) override {
        if (!ok_) return;
        GFX_CPU_SCOPE(ctx.profiler, "Audio update");
        const AudioSettings s = read();
        if (!(s == last_)) {
            sys_.applySettings(s);
            last_ = s;
            demo_.click(sys_);
        }
        Listener l;
        const glm::vec3 p = ctx.camera.position, f = ctx.camera.forward(), u = ctx.camera.up();
        l.position = {p.x, p.y, p.z};
        l.forward = {f.x, f.y, f.z};
        l.up = {u.x, u.y, u.z};
        const float dt = std::max(ctx.dt, 1e-4f);
        lastDt_ = dt;
        if (hasLast_) l.velocity = (l.position - lastPos_) * (1.0f / dt);
        // Teleports (resets, scene loads) would read as a supersonic Doppler sweep.
        if (length(l.velocity) > 100.0f) l.velocity = {};
        lastPos_ = l.position;
        hasLast_ = true;
        sys_.setListener(l);
        sys_.setWindowFocused(ctx.engine.windowFocused());
        demo_.update(sys_, ctx.dt);
        sys_.update(ctx.dt);

        // The mixer runs on the audio device thread, so frame CPU time does not see it:
        // report it as counters (the settings menu shows these next to frame stats).
        const AudioStats st = sys_.stats();
        ctx.profiler.counter("audio.cpu_pct", st.callbackCpuPct);
        ctx.profiler.counter("audio.stream_cpu_pct", st.streamCpuPct);
        ctx.profiler.counter("audio.ram_mb", double(st.totalBytes) / (1024.0 * 1024.0));
        ctx.profiler.counter("audio.voices_real", st.realVoices);
        ctx.profiler.counter("audio.voices_virtual", st.virtualVoices);
        ctx.profiler.counter("audio.latency_ms", st.bufferLatencyMs);
        ctx.profiler.counter("audio.stream_underruns", double(st.streamUnderruns));
        for (int i = 0; i < kStageCount; ++i)
            ctx.profiler.counter(std::string("audio.stage.") + toString(audio::Stage(i)), st.stageCpuPct[size_t(i)]);
    }

    size_t cpuMemoryBytes() const override { return sys_.stats().totalBytes; }

    // Mixer + decoder threads, converted from "% of one core" to ms per frame so the
    // profiler adds it to frame CPU time (per-setting Δ CPU ms then includes audio).
    double offThreadCpuMs() const override {
        if (!ok_) return 0.0;
        const AudioStats st = sys_.stats();
        return (st.callbackCpuPct + st.streamCpuPct) / 100.0 * double(lastDt_) * 1000.0;
    }

    void onGui(FrameContext& ctx) override {
        if (!ok_) return;
        drawSubtitles(ctx);
        if (viz_->asBool()) drawVisualization(ctx);
        if (stats_->asBool()) drawStats();
    }

private:
    AudioSettings read() const {
        AudioSettings s;
        s.masterVolume = master_->asFloat() / 100.0f;
        for (int b = 0; b < kBusCount; ++b) s.busVolume[size_t(b)] = bus_[b]->asFloat() / 100.0f;
        s.muteAll = mute_->asBool();
        s.deviceIndex = device_->asInt() - 1;
        s.speakers = SpeakerConfig(speakers_->asInt());
        s.lfeCrossoverHz = lfe_->asFloat();
        s.sampleRate = SampleRateSetting(rate_->asInt());
        s.format = SampleFormatSetting(format_->asInt());
        s.dither = dither_->asBool();
        s.bufferFrames = kBufferFrames[std::clamp(bufferFrames_->asInt(), 0, 7)];
        s.periods = periods_->asInt();
        s.quality = QualityPreset::Custom;
        s.maxVoices = voices_->asInt();
        s.resampler = ResamplerQuality(resampler_->asInt());
        s.spatial = SpatialMode(spatial_->asInt());
        s.attenuation = AttenuationModel(atten_->asInt());
        s.rolloff = rolloff_->asFloat();
        s.airAbsorption = air_->asBool();
        s.doppler = doppler_->asBool();
        s.dopplerFactor = dopplerFactor_->asFloat();
        s.occlusion = OcclusionMode(occlusion_->asInt());
        s.reverb = ReverbQuality(reverb_->asInt());
        s.reverbWet = wet_->asFloat();
        s.roomSize = room_->asFloat();
        s.damping = damp_->asFloat();
        s.dynamicRange = DynamicRange(dyn_->asInt());
        s.streamBufferMs = stream_->asInt();
        s.simulateSlowDisk = slowDisk_->asBool();
        s.background = BackgroundAudio(background_->asInt());
        s.duckVolume = duck_->asFloat() / 100.0f;
        s.monoAudio = mono_->asBool();
        s.balance = balance_->asFloat();
        s.subtitles = subs_->asBool();
        s.closedCaptions = captions_->asBool();
        s.speakerNames = names_->asBool();
        s.subtitleScale = subSize_->asFloat();
        s.subtitleBackground = subBg_->asFloat();
        s.soundVisualization = viz_->asBool();
        return s;
    }

    // Listener-relative azimuth in degrees (0 = ahead, +90 = right).
    float azimuthOf(const Vec3& p) const {
        const Listener& l = sys_.listener();
        const Vec3 rel = p - l.position;
        const Vec3 right = cross(l.forward, l.up);
        return std::atan2(dot(rel, right), dot(rel, l.forward)) * 57.29578f;
    }

    void drawSubtitles(FrameContext&) {
        const auto& lines = sys_.subtitles();
        if (lines.empty() || (!subs_->asBool() && !captions_->asBool())) return;
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float scale = subSize_->asFloat();
        const float fontSize = ImGui::GetFontSize() * 1.4f * scale;
        float y = io.DisplaySize.y * 0.86f;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            const Subtitle& s = *it;
            if (s.caption ? !captions_->asBool() : !subs_->asBool()) continue;
            std::string text = s.text;
            if (s.caption && s.hasPosition) {  // direction hint for captions
                const float az = azimuthOf(s.position);
                text = (az < -30 ? "<  " : "") + text + (az > 30 ? "  >" : "");
            }
            std::string speaker = (!s.caption && names_->asBool() && !s.speaker.empty()) ? s.speaker + ": " : "";
            const ImVec2 spk = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, speaker.c_str());
            const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fontSize, io.DisplaySize.x * 0.6f, io.DisplaySize.x * 0.6f, text.c_str());
            const float w = spk.x + sz.x, h = std::max(spk.y, sz.y);
            const ImVec2 p0{(io.DisplaySize.x - w) * 0.5f, y - h};
            const float pad = 8.0f * scale;
            const float alpha = std::min(1.0f, s.remaining / 0.3f);
            dl->AddRectFilled({p0.x - pad, p0.y - pad * 0.5f}, {p0.x + w + pad, p0.y + h + pad * 0.5f},
                              IM_COL32(0, 0, 0, int(255 * subBg_->asFloat() * alpha)), 4.0f);
            if (!speaker.empty()) dl->AddText(ImGui::GetFont(), fontSize, p0, IM_COL32(255, 210, 90, int(255 * alpha)), speaker.c_str());
            dl->AddText(ImGui::GetFont(), fontSize, {p0.x + spk.x, p0.y},
                        s.caption ? IM_COL32(170, 220, 255, int(255 * alpha)) : IM_COL32(255, 255, 255, int(255 * alpha)),
                        text.c_str(), nullptr, io.DisplaySize.x * 0.6f);
            y -= h + pad * 1.5f;
        }
    }

    void drawVisualization(FrameContext&) {
        // A ring around the crosshair; each loud sound event lights an arc at its direction.
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 c{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};
        const float r = std::min(io.DisplaySize.x, io.DisplaySize.y) * 0.18f;
        dl->AddCircle(c, r, IM_COL32(255, 255, 255, 40), 64, 2.0f);
        for (const SoundEvent& e : sys_.soundEvents()) {
            const float az = azimuthOf(e.position) / 57.29578f;
            const float a = std::max(0.0f, 1.0f - e.age / 3.0f);
            const ImU32 col = IM_COL32(255, 120, 60, int(230 * a));
            dl->PathArcTo(c, r, az - 1.5707963f - 0.25f, az - 1.5707963f + 0.25f, 16);
            dl->PathStroke(col, 0, 6.0f);
            const ImVec2 tip{c.x + std::sin(az) * (r + 18.0f), c.y - std::cos(az) * (r + 18.0f)};
            dl->AddText(tip, col, e.label.c_str());
        }
    }

    void drawStats() {
        const AudioStats st = sys_.stats();
        ImGui::SetNextWindowSize({420, 0}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Audio stats")) { ImGui::End(); return; }
        ImGui::Text("Backend: %s", st.backend.c_str());
        ImGui::TextWrapped("Device: %s", st.device.c_str());
        ImGui::TextWrapped("Format: %s", st.format.c_str());
        ImGui::Text("Mix: %d ch @ %u Hz   period %d x %d = %.1f ms", st.channels, st.sampleRate, st.periodFrames, st.periods, st.bufferLatencyMs);
        ImGui::Text("Voices: %d real, %d virtual, %d playing", st.realVoices, st.virtualVoices, st.playingVoices);
        ImGui::Text("Stream: %d ms buffered, %llu underruns", st.streamBufferedMs, (unsigned long long)st.streamUnderruns);
        ImGui::Separator();
        ImGui::Text("Audio thread CPU: %.2f%% of one core (callback), decoder %.2f%%", st.callbackCpuPct, st.streamCpuPct);
        ImGui::Text("Occlusion rays (game thread): %.3f ms / frame", st.occlusionMs);
        if (ImGui::BeginTable("stages", 2, ImGuiTableFlags_RowBg)) {
            for (int i = 0; i < kStageCount; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(toString(audio::Stage(i)));
                ImGui::TableNextColumn(); ImGui::Text("%.3f %%", st.stageCpuPct[size_t(i)]);
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::Text("RAM total: %.1f KB", st.totalBytes / 1024.0);
        for (int i = 0; i < kMemCategoryCount; ++i) ImGui::BulletText("%s: %.1f KB", toString(MemCategory(i)), st.mixerMem[size_t(i)] / 1024.0);
        ImGui::BulletText("Decoded sounds: %.1f KB", st.assetBytes / 1024.0);
        ImGui::BulletText("Stream ring buffers: %.1f KB", st.streamBytes / 1024.0);
        ImGui::BulletText("Device buffer: %.1f KB", st.deviceBufferBytes / 1024.0);
        ImGui::Separator();
        ImGui::Text("Compressor / limiter gain reduction: %.1f dB", st.gainReductionDb);
        for (int c = 0; c < st.channels; ++c) {
            const float db = 20.0f * std::log10(std::max(st.peak[size_t(c)], 1e-5f));
            ImGui::ProgressBar(std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f), {-1, 0}, (std::to_string(c) + ": " + std::to_string(int(db)) + " dBFS").c_str());
        }
        ImGui::End();
    }

    AudioSystem sys_;
    AudioDemo demo_;
    AudioSettings last_;
    bool ok_ = false, hasLast_ = false;
    float lastDt_ = 1.0f / 60.0f;
    Vec3 lastPos_;

    Setting *master_ = nullptr, *bus_[kBusCount]{}, *mute_ = nullptr;
    Setting *device_ = nullptr, *speakers_ = nullptr, *lfe_ = nullptr, *rate_ = nullptr, *format_ = nullptr, *dither_ = nullptr;
    Setting *bufferFrames_ = nullptr, *periods_ = nullptr, *voices_ = nullptr, *resampler_ = nullptr;
    Setting *spatial_ = nullptr, *atten_ = nullptr, *rolloff_ = nullptr, *air_ = nullptr, *doppler_ = nullptr, *dopplerFactor_ = nullptr, *occlusion_ = nullptr;
    Setting *reverb_ = nullptr, *wet_ = nullptr, *room_ = nullptr, *damp_ = nullptr, *dyn_ = nullptr, *stream_ = nullptr, *slowDisk_ = nullptr;
    Setting *background_ = nullptr, *duck_ = nullptr, *mono_ = nullptr, *balance_ = nullptr, *viz_ = nullptr;
    Setting *subs_ = nullptr, *captions_ = nullptr, *names_ = nullptr, *subSize_ = nullptr, *subBg_ = nullptr, *stats_ = nullptr;
};

}  // namespace
}  // namespace gfx

GFX_REGISTER_MODULE(gfx::AudioModule);
