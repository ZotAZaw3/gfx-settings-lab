// Anti-aliasing method + upscaling + ray reconstruction, everything that runs in Stage::Upscale.
//   * jitter (Halton) for TAA / temporal upscaling / RR-lite
//   * TAA at native resolution                     docs/taa.md
//   * Temporal upscaler (TAAU, FSR2-style)         docs/upscaling.md
//   * Spatial upscalers: FSR1-style EASU, NIS-lite docs/upscaling.md
//   * RR-lite joint denoise + upscale              docs/ray_reconstruction.md
//   * MSAA sample-count setting (core Forward path does the MSAA rendering)  docs/msaa.md
// Vendor SDKs (DLSS, FSR 3, XeSS, DLSS RR) are listed but greyed out: they need D3D12/Vulkan
// (DLSS, FSR 3) or their SDKs, which this OpenGL 4.5 / D3D11 prototype does not ship.
#include <algorithm>
#include <cmath>

#include "modules/aa/aa_util.h"

namespace gfx::aa {

namespace {
struct TaaParams    { float jitterUV[4]; float gamma, minAlpha, maxAlpha; int reset; int debug; int pad[3]; };
struct TaauParams   { float render[4], output[4], jitter[4]; float gamma, minAlpha, maxAlpha; int reset;
                      int useReactive, debug; float kernelScale; int pad1; };
struct RrParams     { float render[4], output[4], jitter[4]; float maxHistory, depthTol; int normalEncoded, reset;
                      int debug, pad0, pad1, pad2; };
struct EasuParams   { float inputSize[4]; float viewportUV[4]; };
struct SharpenParams{ float sharpness; int split; float splitX; float range; };
static_assert(sizeof(TaaParams) == 48 && sizeof(TaauParams) == 80 && sizeof(RrParams) == 80, "std140 mirror");
}  // namespace

class TemporalAAModule : public Module {
public:
    const char* name() const override { return "aa.temporal"; }

    void registerSettings(SettingsRegistry& s) override {
        method_ = &s.addEnum({.id = kMethod, .label = "Anti-Aliasing", .category = kCategory, .group = "Anti-Aliasing",
                              .tooltip = "Post-process (FXAA, SMAA) or temporal (TAA) anti-aliasing. TAA also removes "
                                         "shimmer but can blur or ghost.",
                              .doc = "taa"},
                             {"Off", "FXAA", "SMAA", "TAA"}, MethodTAA)
                      .presets(MethodFXAA, MethodSMAA, MethodTAA, MethodTAA);
        msaa_ = &s.addEnum({.id = kMsaa, .label = "MSAA", .category = kCategory, .group = "Anti-Aliasing",
                            .tooltip = "Multisample AA: tests geometry coverage at 2-8 points per pixel. Uses the "
                                       "forward renderer; geometry edges only.",
                            .doc = "msaa", .flags = SettingRequiresResize},
                           {"Off", "2x", "4x", "8x"}, 0)
                    .presets(0, 0, 0, 0);
        taaClip_ = &s.addFloat({.id = "aa.taa.clip", .label = "TAA History Clip", .category = kCategory,
                                .group = "Anti-Aliasing",
                                .tooltip = "Width of the colour box history is clipped to (variance clipping, in "
                                           "standard deviations). Lower = less ghosting, more flicker.",
                                .doc = "taa"}, 1.25f, 0.5f, 3.0f)
                       .visible([this] { return temporalActive(); });
        taaHistory_ = &s.addFloat({.id = "aa.taa.history", .label = "TAA History Weight", .category = kCategory,
                                   .group = "Anti-Aliasing",
                                   .tooltip = "How much of the accumulated history is kept each frame. Higher = "
                                              "smoother and more stable, but slower to react.",
                                   .doc = "taa"}, 0.92f, 0.5f, 0.98f)
                          .visible([this] { return temporalActive(); });

        upscaler_ = &s.addEnum({.id = kUpscaler, .label = "Upscaler", .category = kCategory, .group = "Upscaling",
                                .tooltip = "Render at a lower resolution and reconstruct the output image. Temporal "
                                           "upscalers recover real detail; spatial ones only sharpen.",
                                .doc = "upscaling"},
                               {"Off (Native)", "FSR 1-style (spatial, ours)", "NIS-lite (spatial, ours)",
                                "Temporal TAAU (FSR 2-style, ours)", "DLSS (needs D3D12 + SDK)",
                                "FSR 3 (needs D3D12/Vulkan + SDK)", "XeSS (needs SDK)"}, UpOff)
                        .presets(UpTemporal, UpTemporal, UpOff, UpOff)
                        .changed([this](Setting& st) { if (st.i >= UpDLSS) st.i = UpTemporal; });   // SDKs absent
        quality_ = &s.addEnum({.id = kQuality, .label = "Upscaler Quality", .category = kCategory, .group = "Upscaling",
                               .tooltip = "Internal resolution per axis. Native AA = DLAA / FSR Native AA (100%).",
                               .doc = "upscaling"},
                              {"Native AA (100%)", "Ultra Quality (77%)", "Quality (67%)", "Balanced (58%)",
                               "Performance (50%)", "Ultra Performance (33%)"}, 2)
                       .presets(4, 3, 2, 2)
                       .visible([this] { return upscaler_->asInt() != UpOff; });
        rr_ = &s.addEnum({.id = kRayRecon, .label = "Ray Reconstruction", .category = kCategory, .group = "Upscaling",
                          .tooltip = "Replace the ray-tracing denoiser + upscaler with one joint reconstruction pass "
                                     "(RR-lite is our educational stand-in for DLSS Ray Reconstruction).",
                          .doc = "ray_reconstruction"},
                         {"Off (denoiser + upscaler)", "RR-lite (ours)", "DLSS Ray Reconstruction (needs SDK)"}, 0)
                  .presets(0, 0, 0, 0)
                  .changed([](Setting& st) { if (st.i == 2) st.i = 1; });
        debug_ = &s.addEnum({.id = kDebugView, .label = "AA Debug View", .category = "Debug", .group = "Anti-Aliasing",
                             .tooltip = "Visualise motion vectors and the internals of TAA, the upscaler, RR-lite "
                                        "and frame generation.",
                             .doc = "motion_vectors", .flags = SettingNoPreset},
                            {"Off", "Motion vectors", "History only", "History rejection", "Upscaler confidence",
                             "RR-lite history length", "Frame gen source"}, 0);
    }

    void init(Engine& e) override {
        engine_ = &e;
        // Apply the saved upscaler mode before the first frame so startup doesn't render one frame at
        // native size and then resize.
        lastScaleKey_ = upscaler_->asInt() == UpOff ? -1 : quality_->asInt();
        e.upscalerRenderScale = lastScaleKey_ >= 0 ? kQualityScale[lastScaleKey_] : 0.0f;
        e.requestResize = true;
        taa_  = e.createFullscreenPipeline("aa/taa_resolve.frag");
        taau_ = e.createFullscreenPipeline("aa/taau_accumulate.frag");
        rr_p_ = e.createFullscreenPipeline("aa/rr_lite.frag");
        easu_ = e.createFullscreenPipeline("aa/fsr1_easu.frag");
        nis_  = e.createFullscreenPipeline("aa/nis_lite.frag");
        taaParams_  = makeParams(e, sizeof(TaaParams), "aa.taa.params", name());
        taauParams_ = makeParams(e, sizeof(TaauParams), "aa.taau.params", name());
        rrParams_   = makeParams(e, sizeof(RrParams), "aa.rr.params", name());
        easuParams_ = makeParams(e, sizeof(EasuParams), "aa.easu.params", name());
        nisParams_  = makeParams(e, sizeof(SharpenParams), "aa.nis.params", name());
        uint32_t maxS = e.device().caps().maxSamples;
        msaa_->enabled([maxS] { return maxS >= 2; });
    }

    void resize(Engine& e, Extent, Extent) override {
        auto& t = e.targets();
        for (int i = 0; i < 2; ++i) {
            const char* hn[2] = {"aa.history0", "aa.history1"};
            const char* rn[2] = {"aa.rr.history0", "aa.rr.history1"};
            const char* gn[2] = {"aa.rr.guide0", "aa.rr.guide1"};
            history_[i]   = t.create(hn[i], targetDesc(rhi::Format::RGBA16F, hn[i], name()), TargetScale::Output);
            rrHistory_[i] = t.create(rn[i], targetDesc(rhi::Format::RGBA16F, rn[i], name()), TargetScale::Output);
            rrGuide_[i]   = t.create(gn[i], targetDesc(rhi::Format::RGBA16F, gn[i], name()), TargetScale::Output);
        }
        reset_ = true;
    }

    void update(FrameContext& ctx) override {
        // Upscaler quality mode owns the render scale while an upscaler is selected.
        int up = upscaler_->asInt();
        int key = up == UpOff ? -1 : quality_->asInt();
        if (key != lastScaleKey_) {
            // 0 = no upscaler: core falls back to the "display.renderScale" setting. Dynamic resolution
            // (core) multiplies on top of whichever is active.
            ctx.engine.upscalerRenderScale = key >= 0 ? kQualityScale[key] : 0.0f;
            ctx.engine.requestResize = true;
            lastScaleKey_ = key;
        }
        int mode = currentMode(ctx);
        if (mode != lastMode_) { reset_ = true; lastMode_ = mode; }
    }

    void addPasses(PassList& p) override {
        // Jitter before core finalises FrameConstants (order 1000).
        p.add(Stage::FrameSetup, "AA jitter", [this](FrameContext& ctx) {
            if (!temporalActive()) { ctx.camera.setJitter(glm::vec2(0.0f)); return; }
            uint32_t n = jitterPhases(ctx.renderSize, ctx.outputSize);
            uint32_t i = uint32_t(ctx.frameIndex % n) + 1;
            ctx.camera.setJitter(glm::vec2(halton(i, 2) - 0.5f, halton(i, 3) - 0.5f));
        }, 500.0f);

        p.add(Stage::Upscale, "Upscale / TAA", [this](FrameContext& ctx) { run(ctx); }, 0.0f,
              [this] { return method_->asInt() == MethodTAA || upscaler_->asInt() != UpOff || rr_->asInt() != 0; });
    }

private:
    enum Mode { ModeNone, ModeTAA, ModeTAAU, ModeRR, ModeEASU, ModeNIS };

    bool temporalActive() const {
        int up = upscaler_->asInt();
        return method_->asInt() == MethodTAA || up == UpTemporal || up >= UpDLSS || rr_->asInt() != 0;
    }

    int currentMode(const FrameContext& ctx) const {
        int up = upscaler_->asInt();
        if (rr_->asInt() != 0 && ctx.targets.get("rt.noisy")) return ModeRR;
        if (up == UpFSR1) return ModeEASU;
        if (up == UpNIS) return ModeNIS;
        if (up == UpTemporal || up >= UpDLSS || rr_->asInt() != 0) return ModeTAAU;
        if (method_->asInt() == MethodTAA)
            return ctx.renderSize == ctx.outputSize ? ModeTAA : ModeTAAU;   // TAA + render scale = TAAU
        return ModeNone;
    }

    void run(FrameContext& ctx) {
        int mode = currentMode(ctx);
        if (mode == ModeNone) return;
        rhi::Texture* src = ctx.post.src();
        rhi::Texture* dst = ctx.post.dst();
        int dbg = debug_->asInt();
        float minA = 1.0f - taaHistory_->asFloat();
        glm::vec2 j = ctx.camera.jitterPixels;
        auto setSizes = [&](float* r, float* o, float* jt) {
            r[0] = float(ctx.renderSize.width); r[1] = float(ctx.renderSize.height); r[2] = 1.0f / r[0]; r[3] = 1.0f / r[1];
            o[0] = float(ctx.outputSize.width); o[1] = float(ctx.outputSize.height); o[2] = 1.0f / o[0]; o[3] = 1.0f / o[1];
            jt[0] = j.x; jt[1] = kJitterUvYSign * j.y; jt[2] = 0; jt[3] = 0;
        };
        rhi::Texture* velocity = ctx.targets.get("scene.velocity");
        rhi::Texture* depth = ctx.targets.get("scene.depth");
        int cur = int(ctx.frameIndex & 1), prev = cur ^ 1;

        switch (mode) {
        case ModeTAA: {
            TaaParams pr{};
            pr.jitterUV[0] = j.x / float(ctx.renderSize.width);
            pr.jitterUV[1] = kJitterUvYSign * j.y / float(ctx.renderSize.height);
            pr.gamma = taaClip_->asFloat(); pr.minAlpha = minA; pr.maxAlpha = std::max(minA, 0.3f);
            pr.reset = reset_ ? 1 : 0;
            pr.debug = dbg == DbgHistory ? 1 : dbg == DbgRejection ? 2 : 0;
            ctx.cmd.updateBuffer(taaParams_.get(), pr);
            ctx.cmd.bindUniformBuffer(3, taaParams_.get());
            ctx.cmd.bindTexture(0, src, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(1, history_[prev], ctx.engine.linearClamp());
            ctx.cmd.bindTexture(2, velocity, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(3, depth, ctx.engine.pointClamp());
            ctx.fullscreen(taa_.get(), history_[cur], "TAA resolve");
            ctx.cmd.copyTexture(history_[cur], dst);
            break;
        }
        case ModeTAAU: {
            TaauParams pr{};
            setSizes(pr.render, pr.output, pr.jitter);
            // Tuned on the headless quality test (tests/aa): wider clip + longer history than native TAA,
            // because each output pixel only gets a close sample every few frames.
            pr.gamma = taaClip_->asFloat() * 1.6f; pr.minAlpha = std::max(0.02f, minA * 0.25f);
            pr.maxAlpha = 0.25f; pr.reset = reset_ ? 1 : 0; pr.kernelScale = 0.75f;
            rhi::Texture* reactive = ctx.targets.get("aa.reactive");   // optional, written by particles
            pr.useReactive = reactive ? 1 : 0;
            pr.debug = dbg == DbgConfidence ? 1 : dbg == DbgRejection ? 2 : 0;
            ctx.cmd.updateBuffer(taauParams_.get(), pr);
            ctx.cmd.bindUniformBuffer(3, taauParams_.get());
            ctx.cmd.bindTexture(0, src, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(1, history_[prev], ctx.engine.linearClamp());
            ctx.cmd.bindTexture(2, velocity, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(3, depth, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(4, reactive ? reactive : depth, ctx.engine.pointClamp());
            ctx.fullscreen(taau_.get(), history_[cur], "Temporal upscale (TAAU)");
            ctx.cmd.copyTexture(history_[cur], dst);
            break;
        }
        case ModeRR: {
            RrParams pr{};
            setSizes(pr.render, pr.output, pr.jitter);
            pr.maxHistory = 32.0f; pr.depthTol = 0.05f; pr.normalEncoded = 0; pr.reset = reset_ ? 1 : 0;
            pr.debug = dbg == DbgRrHistory ? 1 : 0;
            rhi::Texture* albedo = ctx.targets.get("rt.albedo");
            if (!albedo) albedo = ctx.targets.get("gbuffer.albedo");
            ctx.cmd.updateBuffer(rrParams_.get(), pr);
            ctx.cmd.bindUniformBuffer(3, rrParams_.get());
            ctx.cmd.bindTexture(0, src, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(1, rrHistory_[prev], ctx.engine.linearClamp());
            ctx.cmd.bindTexture(2, velocity, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(3, depth, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(4, ctx.targets.get("gbuffer.normal"), ctx.engine.pointClamp());
            ctx.cmd.bindTexture(5, rrGuide_[prev], ctx.engine.pointClamp());
            ctx.cmd.bindTexture(6, albedo, ctx.engine.pointClamp());
            drawFullscreenMRT(ctx, rr_p_.get(), {rrHistory_[cur], rrGuide_[cur], dst}, "RR-lite (denoise + upscale)");
            break;
        }
        case ModeEASU: {
            EasuParams pr{};
            pr.inputSize[0] = float(src->width()); pr.inputSize[1] = float(src->height());
            pr.inputSize[2] = 1.0f / pr.inputSize[0]; pr.inputSize[3] = 1.0f / pr.inputSize[1];
            pr.viewportUV[0] = pr.viewportUV[1] = 1.0f;
            ctx.cmd.updateBuffer(easuParams_.get(), pr);
            ctx.cmd.bindUniformBuffer(3, easuParams_.get());
            ctx.cmd.bindTexture(0, src, ctx.engine.pointClamp());
            ctx.fullscreen(easu_.get(), dst, "FSR1-style EASU");
            break;
        }
        case ModeNIS: {
            SharpenParams pr{0.5f, 0, 0.5f, 1.0f};
            ctx.cmd.updateBuffer(nisParams_.get(), pr);
            ctx.cmd.bindUniformBuffer(3, nisParams_.get());
            ctx.cmd.bindTexture(0, src, ctx.engine.linearClamp());
            ctx.fullscreen(nis_.get(), dst, "NIS-lite scale");
            break;
        }
        }
        ctx.post.swap();
        reset_ = false;
    }

    Engine* engine_ = nullptr;
    Setting *method_ = nullptr, *msaa_ = nullptr, *taaClip_ = nullptr, *taaHistory_ = nullptr;
    Setting *upscaler_ = nullptr, *quality_ = nullptr, *rr_ = nullptr, *debug_ = nullptr;
    rhi::PipelinePtr taa_, taau_, rr_p_, easu_, nis_;
    rhi::BufferPtr taaParams_, taauParams_, rrParams_, easuParams_, nisParams_;
    rhi::Texture* history_[2] = {};
    rhi::Texture* rrHistory_[2] = {};
    rhi::Texture* rrGuide_[2] = {};
    bool reset_ = true;
    int lastScaleKey_ = -2, lastMode_ = -1;
};
GFX_REGISTER_MODULE(TemporalAAModule);

}  // namespace gfx::aa
