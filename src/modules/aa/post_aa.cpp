// Post-process AA and sharpening at output resolution (Stage::PostLDR), plus the motion-vector debug view.
//   FXAA                               docs/fxaa.md
//   SMAA 1x (analytic, simplified)     docs/smaa.md
//   CAS / RCAS / NIS-lite / unsharp    docs/sharpening_cas.md
#include <algorithm>

#include "modules/aa/aa_util.h"

namespace gfx::aa {

namespace {
struct FxaaParams    { float rcp[4]; float edgeThreshold, edgeThresholdMin, subpix; int steps; float range, pad[3]; };
struct SmaaParams    { float rcp[4]; float threshold; int maxSearch; int diag; float contrastFactor; };
struct SharpenParams { float sharpness; int split; float splitX; float range; };
struct DebugParams   { float size[4]; float scale, pad[3]; };
static_assert(sizeof(FxaaParams) == 48 && sizeof(SmaaParams) == 32, "std140 mirror");
}  // namespace

class PostAAModule : public Module {
public:
    const char* name() const override { return "aa.post"; }

    void registerSettings(SettingsRegistry& s) override {
        fxaaQ_ = &s.addEnum({.id = "aa.fxaa.quality", .label = "FXAA Quality", .category = kCategory,
                             .group = "Anti-Aliasing",
                             .tooltip = "Edge search length and threshold. Higher finds longer edges, costs a little more.",
                             .doc = "fxaa"}, {"Low", "Medium", "High"}, 2)
                     .presets(0, 1, 2, 2)
                     .visible([&s] { return s.getInt(kMethod) == MethodFXAA; });
        smaaQ_ = &s.addEnum({.id = "aa.smaa.quality", .label = "SMAA Quality", .category = kCategory,
                             .group = "Anti-Aliasing",
                             .tooltip = "Edge threshold and how far SMAA searches along an edge to reconstruct it.",
                             .doc = "smaa"}, {"Low", "Medium", "High", "Ultra"}, 2)
                     .presets(0, 1, 2, 3)
                     .visible([&s] { return s.getInt(kMethod) == MethodSMAA; });
        sharpen_ = &s.addEnum({.id = kSharpen, .label = "Sharpening", .category = kCategory, .group = "Sharpening",
                               .tooltip = "Restores detail softened by TAA and upscaling. CAS adapts to local contrast "
                                          "to avoid halos; unsharp mask is the naive version for comparison.",
                               .doc = "sharpening_cas"},
                              {"Off", "CAS", "RCAS (FSR)", "NIS-lite", "Unsharp mask"}, 1)
                       .presets(0, 1, 1, 1);
        strength_ = &s.addFloat({.id = "aa.sharpen.strength", .label = "Sharpness", .category = kCategory,
                                 .group = "Sharpening", .tooltip = "0 = subtle, 1 = strong.",
                                 .doc = "sharpening_cas"}, 0.5f, 0.0f, 1.0f)
                        .visible([this, &s] { return sharpen_->asInt() != 0 || s.getInt(kUpscaler) == UpFSR1; });
        split_ = &s.addBool({.id = "aa.sharpen.split", .label = "Sharpening Split View", .category = "Debug",
                             .group = "Anti-Aliasing", .tooltip = "Left half unsharpened, right half sharpened.",
                             .doc = "sharpening_cas", .flags = SettingNoPreset | SettingDebug}, false);
        settings_ = &s;
    }

    void init(Engine& e) override {
        fxaa_ = e.createFullscreenPipeline("aa/fxaa.frag");
        smaaEdge_ = e.createFullscreenPipeline("aa/smaa_edge.frag");
        smaaWeights_ = e.createFullscreenPipeline("aa/smaa_weights.frag");
        smaaBlend_ = e.createFullscreenPipeline("aa/smaa_blend.frag");
        sharpenP_[1] = e.createFullscreenPipeline("aa/cas.frag");
        sharpenP_[2] = e.createFullscreenPipeline("aa/rcas.frag");
        sharpenP_[3] = e.createFullscreenPipeline("aa/nis_lite.frag");
        sharpenP_[4] = e.createFullscreenPipeline("aa/unsharp.frag");
        debugMv_ = e.createFullscreenPipeline("aa/debug_velocity.frag");
        fxaaParams_ = makeParams(e, sizeof(FxaaParams), "aa.fxaa.params", name());
        smaaParams_ = makeParams(e, sizeof(SmaaParams), "aa.smaa.params", name());
        sharpParams_ = makeParams(e, sizeof(SharpenParams), "aa.sharpen.params", name());
        debugParams_ = makeParams(e, sizeof(DebugParams), "aa.debug.params", name());
    }

    void resize(Engine& e, Extent, Extent) override {
        edges_ = e.targets().create("aa.smaa.edges", targetDesc(rhi::Format::RG8, "aa.smaa.edges", name()),
                                    TargetScale::Output);
        weights_ = e.targets().create("aa.smaa.weights", targetDesc(rhi::Format::RGBA8, "aa.smaa.weights", name()),
                                      TargetScale::Output);
    }

    void addPasses(PassList& p) override {
        // Early in PostLDR: AA before vignette / grain / CA so they aren't treated as edges.
        p.add(Stage::PostLDR, "FXAA", [this](FrameContext& ctx) { runFxaa(ctx); }, -50.0f,
              [this] { return settings_->getInt(kMethod) == MethodFXAA; });
        p.add(Stage::PostLDR, "SMAA", [this](FrameContext& ctx) { runSmaa(ctx); }, -50.0f,
              [this] { return settings_->getInt(kMethod) == MethodSMAA; });
        p.add(Stage::PostLDR, "Sharpening", [this](FrameContext& ctx) { runSharpen(ctx); }, -40.0f,
              [this] { return sharpen_->asInt() != 0 || settings_->getInt(kUpscaler) == UpFSR1; });
        p.add(Stage::PostLDR, "Motion vector view", [this](FrameContext& ctx) { runDebug(ctx); }, 1000.0f,
              [this] { return settings_->getInt(kDebugView) == DbgMotion; });
    }

private:
    void runFxaa(FrameContext& ctx) {
        static const float thr[3] = {0.25f, 0.166f, 0.125f}, sub[3] = {0.5f, 0.75f, 1.0f};
        static const int steps[3] = {4, 8, 12};
        int q = fxaaQ_->asInt();
        rhi::Texture* src = ctx.post.src();
        FxaaParams pr{};
        pr.rcp[0] = 1.0f / float(src->width()); pr.rcp[1] = 1.0f / float(src->height());
        pr.edgeThreshold = thr[q]; pr.edgeThresholdMin = 0.0312f; pr.subpix = sub[q]; pr.steps = steps[q];
        pr.range = ldrRange(ctx);
        ctx.cmd.updateBuffer(fxaaParams_.get(), pr);
        ctx.cmd.bindUniformBuffer(3, fxaaParams_.get());
        ctx.cmd.bindTexture(0, src, ctx.engine.linearClamp());
        ctx.fullscreen(fxaa_.get(), ctx.post.dst(), "FXAA");
        ctx.post.swap();
    }

    void runSmaa(FrameContext& ctx) {
        static const float thr[4] = {0.15f, 0.1f, 0.1f, 0.05f};
        static const int search[4] = {4, 8, 16, 32};
        int q = smaaQ_->asInt();
        rhi::Texture* src = ctx.post.src();
        SmaaParams pr{};
        pr.rcp[0] = 1.0f / float(src->width()); pr.rcp[1] = 1.0f / float(src->height()); pr.rcp[2] = ldrRange(ctx);
        pr.threshold = thr[q]; pr.maxSearch = search[q]; pr.diag = q >= 2; pr.contrastFactor = 2.0f;
        ctx.cmd.updateBuffer(smaaParams_.get(), pr);
        ctx.cmd.bindUniformBuffer(3, smaaParams_.get());
        ctx.cmd.bindTexture(0, src, ctx.engine.pointClamp());
        drawFullscreenMRT(ctx, smaaEdge_.get(), {edges_}, "SMAA edges", /*clear*/ true);   // shader discards non-edges
        ctx.cmd.bindTexture(0, edges_, ctx.engine.pointClamp());
        ctx.fullscreen(smaaWeights_.get(), weights_, "SMAA weights");
        ctx.cmd.bindTexture(0, src, ctx.engine.pointClamp());
        ctx.cmd.bindTexture(1, weights_, ctx.engine.pointClamp());
        ctx.fullscreen(smaaBlend_.get(), ctx.post.dst(), "SMAA blend");
        ctx.post.swap();
    }

    void runSharpen(FrameContext& ctx) {
        int mode = sharpen_->asInt();
        if (mode == 0) mode = 2;   // FSR 1-style upscaling: RCAS is part of the algorithm
        SharpenParams pr{strength_->asFloat(), split_->asBool() ? 1 : 0, 0.5f, ldrRange(ctx)};
        ctx.cmd.updateBuffer(sharpParams_.get(), pr);
        ctx.cmd.bindUniformBuffer(3, sharpParams_.get());
        ctx.cmd.bindTexture(0, ctx.post.src(), mode == 3 ? ctx.engine.linearClamp() : ctx.engine.pointClamp());
        ctx.fullscreen(sharpenP_[mode].get(), ctx.post.dst(), "Sharpen");
        ctx.post.swap();
    }

    void runDebug(FrameContext& ctx) {
        DebugParams pr{};
        pr.size[0] = float(ctx.renderSize.width); pr.size[1] = float(ctx.renderSize.height);
        pr.scale = 0.1f;   // 10 px/frame = full brightness
        ctx.cmd.updateBuffer(debugParams_.get(), pr);
        ctx.cmd.bindUniformBuffer(3, debugParams_.get());
        ctx.cmd.bindTexture(2, ctx.targets.get("scene.velocity"), ctx.engine.linearClamp());
        ctx.fullscreen(debugMv_.get(), ctx.post.dst(), "Motion vector view");
        ctx.post.swap();
    }

    SettingsRegistry* settings_ = nullptr;
    Setting *fxaaQ_ = nullptr, *smaaQ_ = nullptr, *sharpen_ = nullptr, *strength_ = nullptr, *split_ = nullptr;
    rhi::PipelinePtr fxaa_, smaaEdge_, smaaWeights_, smaaBlend_, sharpenP_[5], debugMv_;
    rhi::BufferPtr fxaaParams_, smaaParams_, sharpParams_, debugParams_;
    rhi::Texture *edges_ = nullptr, *weights_ = nullptr;
};
GFX_REGISTER_MODULE(PostAAModule);

}  // namespace gfx::aa
