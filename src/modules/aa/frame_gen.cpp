// Frame generation (2x interpolation, ours) in Stage::FrameGen.  Doc: docs/frame_generation.md
//   1. optical flow at quarter resolution, refining game motion vectors (fg_optical_flow.frag)
//   2. interpolate the frame half-way between N-1 and N (fg_interpolate.frag)
//   3. hand it to core as ctx.interpolatedFrame: presented BEFORE the real frame N, UI drawn on both.
// DLSS FG and FSR 3 FG need D3D12/Vulkan and their SDKs; they're listed greyed out.
#include "modules/aa/aa_util.h"

namespace gfx::aa {

namespace {
struct FlowParams   { float flowSize[4]; int searchRadius; float lod; int quality; int pad; };
struct InterpParams { float t, depthSharpness; int debug; int flowIters; };
}  // namespace

class FrameGenModule : public Module {
public:
    const char* name() const override { return "aa.framegen"; }

    void registerSettings(SettingsRegistry& s) override {
        mode_ = &s.addEnum({.id = kFrameGen, .label = "Frame Generation", .category = kCategory,
                            .group = "Frame Generation",
                            .tooltip = "Insert a generated frame between every two rendered frames: smoother motion, "
                                       "but input latency does not improve. Works best above 60 fps.",
                            .doc = "frame_generation", .flags = SettingNoPreset},
                           {"Off", "2x Interpolation (ours)", "DLSS Frame Generation (needs D3D12 + SDK)",
                            "FSR 3 Frame Generation (needs D3D12/Vulkan + SDK)"}, 0)
                    .changed([](Setting& st) { if (st.i >= 2) st.i = 1; });
        search_ = &s.addInt({.id = "aa.framegen.flow_radius", .label = "Optical Flow Search", .category = kCategory,
                             .group = "Frame Generation",
                             .tooltip = "Search radius (quarter-res pixels) around the game motion vector. Catches "
                                        "shadows and reflections that motion vectors miss.",
                             .doc = "frame_generation", .flags = SettingNoPreset}, 2, 0, 4)
                      .visible([this] { return mode_->asInt() != 0; });
        genOnly_ = &s.addBool({.id = "aa.framegen.generated_only", .label = "Show Generated Frames Only",
                               .category = "Debug", .group = "Frame Generation",
                               .tooltip = "Replace every real frame with the generated one to inspect artefacts.",
                               .doc = "frame_generation", .flags = SettingNoPreset | SettingDebug}, false);
        settings_ = &s;
    }

    void init(Engine& e) override {
        flow_ = e.createFullscreenPipeline("aa/fg_optical_flow.frag");
        interp_ = e.createFullscreenPipeline("aa/fg_interpolate.frag");
        flowParams_ = makeParams(e, sizeof(FlowParams), "aa.fg.flow.params", name());
        interpParams_ = makeParams(e, sizeof(InterpParams), "aa.fg.interp.params", name());
    }

    void resize(Engine& e, Extent, Extent) override {
        auto& t = e.targets();
        prevFrame_ = t.create("aa.fg.prevFrame", targetDesc(rhi::Format::RGBA16F, "aa.fg.prevFrame", name()),
                              TargetScale::Output);
        prevDepth_ = t.create("aa.fg.prevDepth",
                              targetDesc(rhi::Format::D32F, "aa.fg.prevDepth", name(),
                                         rhi::TexUsageSampled | rhi::TexUsageDepthStencil), TargetScale::Render);
        flowTex_ = t.create("aa.fg.flow", targetDesc(rhi::Format::RGBA16F, "aa.fg.flow", name()),
                            TargetScale::Output, 0.25f);
        generated_ = t.create("aa.fg.generated", targetDesc(rhi::Format::RGBA16F, "aa.fg.generated", name()),
                              TargetScale::Output);
        havePrev_ = false;   // sizes changed: last frame can't be reused
    }

    void addPasses(PassList& p) override {
        p.add(Stage::FrameGen, "Frame generation", [this](FrameContext& ctx) { run(ctx); }, 0.0f,
              [this] { return mode_->asInt() != 0; });
    }

private:
    void run(FrameContext& ctx) {
        rhi::Texture* cur = ctx.targets.get("final.output");
        rhi::Texture* depth = ctx.targets.get("scene.depth");
        rhi::Texture* velocity = ctx.targets.get("scene.velocity");
        if (!cur || !depth || !velocity) return;
        // A large frame gap (pause, alt-tab, camera cut via reset) makes interpolation meaningless.
        if (havePrev_ && ctx.frameIndex == lastFrame_ + 1) {
            FlowParams fp{};
            fp.flowSize[0] = float(flowTex_->width()); fp.flowSize[1] = float(flowTex_->height());
            fp.flowSize[2] = 1.0f / fp.flowSize[0]; fp.flowSize[3] = 1.0f / fp.flowSize[1];
            fp.searchRadius = search_->asInt(); fp.lod = 0.0f;
            ctx.cmd.updateBuffer(flowParams_.get(), fp);
            ctx.cmd.bindUniformBuffer(3, flowParams_.get());
            ctx.cmd.bindTexture(0, cur, ctx.engine.linearClamp());
            ctx.cmd.bindTexture(1, prevFrame_, ctx.engine.linearClamp());
            ctx.cmd.bindTexture(2, velocity, ctx.engine.linearClamp());
            ctx.fullscreen(flow_.get(), flowTex_, "FG optical flow");

            InterpParams ip{0.5f, 4.0f, settings_->getInt(kDebugView) == DbgFgSource ? 1 : 0, 2};
            ctx.cmd.updateBuffer(interpParams_.get(), ip);
            ctx.cmd.bindUniformBuffer(3, interpParams_.get());
            ctx.cmd.bindTexture(0, prevFrame_, ctx.engine.linearClamp());
            ctx.cmd.bindTexture(1, cur, ctx.engine.linearClamp());
            ctx.cmd.bindTexture(2, flowTex_, ctx.engine.pointClamp());   // never blend motions
            ctx.cmd.bindTexture(3, prevDepth_, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(4, depth, ctx.engine.pointClamp());
            ctx.fullscreen(interp_.get(), generated_, "FG interpolate");
        }
        bool ready = havePrev_ && ctx.frameIndex == lastFrame_ + 1;
        // Keep the REAL frame N as next frame's N-1 before (optionally) overwriting final.output.
        ctx.cmd.copyTexture(cur, prevFrame_);
        ctx.cmd.copyTexture(depth, prevDepth_);
        havePrev_ = true; lastFrame_ = ctx.frameIndex;
        if (!ready) return;
        if (genOnly_->asBool()) ctx.cmd.copyTexture(generated_, cur);
        else ctx.interpolatedFrame = generated_;
    }

    SettingsRegistry* settings_ = nullptr;
    Setting *mode_ = nullptr, *search_ = nullptr, *genOnly_ = nullptr;
    rhi::PipelinePtr flow_, interp_;
    rhi::BufferPtr flowParams_, interpParams_;
    rhi::Texture *prevFrame_ = nullptr, *prevDepth_ = nullptr, *flowTex_ = nullptr, *generated_ = nullptr;
    bool havePrev_ = false;
    uint64_t lastFrame_ = 0;
};
GFX_REGISTER_MODULE(FrameGenModule);

}  // namespace gfx::aa
