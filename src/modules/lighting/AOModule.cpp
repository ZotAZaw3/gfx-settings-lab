// AOModule ("lighting.ao") - SSAO / HBAO / GTAO into the shared lighting.ao input, bilateral blur,
// temporal accumulation for GTAO, multi-bounce and specular occlusion toggles.
// Docs: docs/ambient_occlusion.md, docs/specular_occlusion.md
#include <algorithm>

#include "core/engine.h"
#include "modules/lighting/LightingShared.h"

namespace gfx::lighting {

class AOModule : public Module {
public:
    const char* name() const override { return "lighting.ao"; }

    void registerSettings(SettingsRegistry& s) override {
        const char* cat = "Lighting";
        mode_ = &s.addEnum({.id = "lighting.ao.mode", .label = "Ambient Occlusion", .category = cat, .group = "Ambient Occlusion",
                            .tooltip = "Darkens creases and contact areas where ambient light is blocked.",
                            .doc = "ambient_occlusion"}, {"Off", "SSAO", "HBAO", "GTAO"}, 3)
                    .presets(1, 1, 2, 3);
        quality_ = &s.addEnum({.id = "lighting.ao.quality", .label = "AO Quality", .category = cat, .group = "Ambient Occlusion",
                               .tooltip = "Samples per pixel and resolution of the AO pass.",
                               .doc = "ambient_occlusion"}, {"Low", "Medium", "High", "Ultra"}, 2)
                       .presets(0, 1, 2, 3)
                       .visible([this] { return mode_->asInt() != 0; });
        radius_ = &s.addFloat({.id = "lighting.ao.radius", .label = "AO Radius", .category = cat, .group = "Ambient Occlusion",
                               .tooltip = "World-space reach of occlusion (meters).", .doc = "ambient_occlusion",
                               .flags = SettingNoPreset}, 0.6f, 0.05f, 3.0f)
                      .fmt("%.2f m")
                      .visible([this] { return mode_->asInt() != 0; });
        intensity_ = &s.addFloat({.id = "lighting.ao.intensity", .label = "AO Intensity", .category = cat, .group = "Ambient Occlusion",
                                  .tooltip = "Exponent on the AO term: 0 = none, 1 = physically based, 1.5-2 = stylized/readable.", .doc = "ambient_occlusion",
                                  .flags = SettingNoPreset}, 1.5f, 0.0f, 4.0f)
                         .visible([this] { return mode_->asInt() != 0; });
        multibounce_ = &s.addBool({.id = "lighting.ao.multibounce", .label = "AO Multi-Bounce", .category = cat,
                                   .group = "Ambient Occlusion", .tooltip = "Light materials lose less light to AO (GTAO fit).",
                                   .doc = "ambient_occlusion"}, true)
                           .presets(0, 0, 1, 1);
        specOcc_ = &s.addBool({.id = "lighting.ao.specularOcclusion", .label = "Specular Occlusion", .category = cat,
                               .group = "Ambient Occlusion", .tooltip = "Fades environment reflections inside occluded creases.",
                               .doc = "specular_occlusion"}, true)
                       .presets(0, 1, 1, 1);
    }

    void init(Engine& e) override {
        auto& sh = e.shaders();
        prog_[1] = sh.compute("lighting/ssao.comp");
        prog_[2] = sh.compute("lighting/hbao.comp");
        prog_[3] = sh.compute("lighting/gtao.comp");
        blur_ = sh.compute("lighting/ao_blur.comp");
        temporal_ = sh.compute("lighting/temporal_accum.comp", {"OUT_FMT=r8"});
        params_ = e.device().createBuffer({.size = 32, .usage = rhi::BufUsageUniform, .name = "ao.params", .owner = name()});
    }

    void update(FrameContext& ctx) override {
        auto& u = shared().ubo;
        int mode = mode_->asInt(), q = quality_->asInt();
        bool rtAO = settingOn(ctx, "rt.ao") && !settingOn(ctx, "rt.gi");
        active_ = mode != 0 && !ctx.replaceLighting && !ctx.msaaForward && !rtAO;
        // samples (SSAO) / directions (HBAO) / slices (GTAO), and march steps
        static const int samples[4][4] = {{0, 0, 0, 0}, {8, 16, 24, 32}, {4, 6, 8, 12}, {2, 2, 3, 4}};
        static const int steps[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {3, 4, 4, 6}, {4, 6, 6, 8}};
        u.aoCfg = glm::ivec4(active_ ? mode : 0, samples[mode][q], steps[mode][q], multibounce_->asBool() ? 1 : 0);
        u.aoParams = glm::vec4(radius_->asFloat(), intensity_->asFloat(), 0.02f, specOcc_->asBool() ? 1.0f : 0.0f);

        float scale = q == 0 ? 0.5f : 1.0f;
        if (active_ && (!out_ || scale != scale_)) {
            if (out_) release(ctx);
            scale_ = scale;
            out_ = ctx.targets.create("lighting.ao", storageDesc(rhi::Format::R8, "lighting.ao", name()), TargetScale::Render, scale);
            raw_ = ctx.targets.create("lighting.aoRaw", storageDesc(rhi::Format::R8, "lighting.aoRaw", name()), TargetScale::Render, scale);
            tmp_ = ctx.targets.create("lighting.aoTemp", storageDesc(rhi::Format::R8, "lighting.aoTemp", name()), TargetScale::Render, scale);
            hist_ = ctx.targets.create("lighting.aoHistory", storageDesc(rhi::Format::R8, "lighting.aoHistory", name()), TargetScale::Render, scale);
            historyValid_ = false;
        } else if (!active_ && out_) {
            release(ctx);
        }
        if (out_) {   // re-fetch: targets are recreated on resize
            out_ = ctx.targets.get("lighting.ao");
            raw_ = ctx.targets.get("lighting.aoRaw");
            tmp_ = ctx.targets.get("lighting.aoTemp");
            hist_ = ctx.targets.get("lighting.aoHistory");
            if (out_ && out_->width() != lastW_) { historyValid_ = false; lastW_ = out_->width(); }
        }
    }

    void addPasses(PassList& p) override {
        p.add(Stage::Lighting, "Ambient occlusion", [this](FrameContext& ctx) { run(ctx); }, 10.0f,
              [this] { return active_ && out_ != nullptr; });
    }

private:
    void release(FrameContext& ctx) {
        for (const char* n : {"lighting.ao", "lighting.aoRaw", "lighting.aoTemp", "lighting.aoHistory"}) ctx.targets.remove(n);
        out_ = raw_ = tmp_ = hist_ = nullptr;
    }

    void dispatch(FrameContext& ctx, rhi::Texture* dst) {
        ctx.cmd.bindImage(0, dst, 0, rhi::Access::Write);
        ctx.cmd.dispatch(groups(dst->width(), 8), groups(dst->height(), 8));
        ctx.cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
    }

    void run(FrameContext& ctx) {
        if (ctx.replaceLighting) return;
        auto& cmd = ctx.cmd;
        rhi::Sampler* pt = ctx.engine.pointClamp();
        rhi::Sampler* lin = ctx.engine.linearClamp();
        rhi::Texture* depth = ctx.targets.get("scene.depth");
        glm::vec2 size(out_->width(), out_->height());
        int mode = shared().ubo.aoCfg.x;

        // 1) kernel
        glm::vec4 a(size, 0, 0);
        cmd.updateBuffer(params_.get(), a);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindComputeProgram(prog_[mode]);
        cmd.bindTexture(1, ctx.targets.get("gbuffer.normal"), pt);
        cmd.bindTexture(3, depth, pt);
        dispatch(ctx, raw_);
        rhi::Texture* src = raw_;

        // 2) temporal accumulation (GTAO rotates its slices every frame)
        if (mode == 3) {
            glm::vec4 ta(size, 0.15f, historyValid_ ? 0.0f : 1.0f);
            cmd.updateBuffer(params_.get(), ta);
            cmd.bindUniformBuffer(3, params_.get());
            cmd.bindComputeProgram(temporal_);
            cmd.bindTexture(3, depth, pt);
            cmd.bindTexture(4, ctx.targets.get("scene.velocity"), pt);
            cmd.bindTexture(5, raw_, pt);
            cmd.bindTexture(6, hist_, lin);
            dispatch(ctx, tmp_);
            cmd.copyTexture(tmp_, hist_);
            historyValid_ = true;
            src = tmp_;
        }

        // 3) separable bilateral blur: H into the free target, V into lighting.ao
        rhi::Texture* mid = (src == raw_) ? tmp_ : raw_;
        int radius = mode == 3 ? 2 : 4;
        struct { glm::vec4 a; glm::ivec4 b; } blur{};
        cmd.bindComputeProgram(blur_);
        cmd.bindTexture(3, depth, pt);
        blur = {glm::vec4(size, 1, 0), glm::ivec4(radius, 0, 0, 0)};
        cmd.updateBuffer(params_.get(), blur);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindTexture(5, src, pt);
        dispatch(ctx, mid);
        blur = {glm::vec4(size, 0, 1), glm::ivec4(radius, 0, 0, 0)};
        cmd.updateBuffer(params_.get(), blur);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindTexture(5, mid, pt);
        dispatch(ctx, out_);
    }

    Setting *mode_ = nullptr, *quality_ = nullptr, *radius_ = nullptr, *intensity_ = nullptr, *multibounce_ = nullptr, *specOcc_ = nullptr;
    rhi::Program* prog_[4] = {};
    rhi::Program *blur_ = nullptr, *temporal_ = nullptr;
    rhi::BufferPtr params_;
    rhi::Texture *out_ = nullptr, *raw_ = nullptr, *tmp_ = nullptr, *hist_ = nullptr;
    float scale_ = 1.0f;
    uint32_t lastW_ = 0;
    bool active_ = false, historyValid_ = false;
};

GFX_REGISTER_MODULE(AOModule);

}  // namespace gfx::lighting
