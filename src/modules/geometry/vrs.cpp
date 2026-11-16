// geometry.vrs — Variable rate shading (content-adaptive shading-rate image + software emulation).
//
//   Translucent order 1 (right after the deferred resolve wrote scene.color):
//     "VRS rate image"  compute: per 16x16 tile contrast + motion -> R32UI rate (1x1 / 2x2 / 4x4)
//     "VRS apply"       broadcast one shaded sample per coarse block (what hardware VRS does to the PS)
// rhi has no hardware shading-rate API (GL 4.5 core / D3D11), so this shows the image-quality side
// of VRS; docs/variable_rate_shading.md explains the hardware path and the expected savings.
#include <algorithm>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

class VrsModule : public Module {
public:
    const char* name() const override { return "geometry.vrs"; }

    void registerSettings(SettingsRegistry& s) override {
        mode_ = &s.addEnum({.id = "performance.vrs", .label = "Variable Rate Shading", .category = "Performance", .group = "Shading",
                            .tooltip = "Shade flat / dark / fast-moving areas once per 2x2 or 4x4 pixels (emulated).",
                            .doc = "variable_rate_shading"}, {"Off", "Quality", "Balanced", "Performance"}, 0)
                    .presets(3, 2, 0, 0);
        overlay_ = &s.addBool({.id = "performance.vrsOverlay", .label = "VRS Overlay", .category = "Performance", .group = "Shading",
                               .tooltip = "Tint tiles: green = 2x2, red = 4x4.", .doc = "variable_rate_shading",
                               .flags = SettingNoPreset | SettingDebug}, false)
                       .visible([this] { return mode_->asInt() > 0; });
    }

    void init(Engine& e) override {
        rateProg_ = e.shaders().compute("geometry/vrs_rate.comp");
        applyPipe_ = e.createFullscreenPipeline("geometry/vrs_apply.frag");
        ubo_ = geo::makeBuffer(e, 16, rhi::BufUsageUniform, "vrs.params", name());
    }

    void resize(Engine& e, Extent render, Extent) override {
        e.targets().remove("geometry.vrsRate");
        rhi::TextureDesc d; d.format = rhi::Format::R32UI; d.usage = rhi::TexUsageSampled | rhi::TexUsageStorage;
        d.width = (render.width + 15) / 16; d.height = (render.height + 15) / 16;
        d.name = "geometry.vrsRate"; d.owner = name();
        rate_ = e.targets().create(d.name, d, TargetScale::Fixed);
        rhi::TextureDesc c; c.format = rhi::Format::RGBA16F; c.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
        c.name = "geometry.vrsColorCopy"; c.owner = name();
        copy_ = e.targets().create(c.name, c, TargetScale::Render);
        tilesX_ = d.width; tilesY_ = d.height;
    }

    void addPasses(PassList& p) override {
        p.add(Stage::Translucent, "VRS", [this](FrameContext& ctx) { run(ctx); }, 1.0f, [this] { return mode_->asInt() > 0; });
    }

private:
    void run(FrameContext& ctx) {
        static const float kThreshold[4] = {0, 0.02f, 0.05f, 0.09f}, kMaxRate[4] = {0, 1, 1, 2}, kMotion[4] = {0, 1e9f, 12.0f, 6.0f};
        int m = std::clamp(mode_->asInt(), 0, 3);
        float prm[4] = {kThreshold[m], kMaxRate[m], kMotion[m], overlay_->asBool() ? 1.0f : 0.0f};
        ctx.cmd.updateBuffer(ubo_.get(), prm, sizeof(prm));
        rhi::Texture* color = ctx.targets.get("scene.color");

        ctx.cmd.bindComputeProgram(rateProg_);
        ctx.cmd.bindUniformBuffer(3, ubo_.get());
        ctx.cmd.bindTexture(0, color, ctx.engine.pointClamp());
        ctx.cmd.bindTexture(1, ctx.targets.get("scene.velocity"), ctx.engine.pointClamp());
        ctx.cmd.bindImage(0, rate_, 0, rhi::Access::Write);
        ctx.cmd.dispatch(tilesX_, tilesY_);
        ctx.cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);

        ctx.cmd.copyTexture(color, copy_);
        rhi::RenderPassDesc rp; rp.colors[0].texture = color; rp.colors[0].load = rhi::LoadOp::DontCare; rp.colorCount = 1;
        rp.name = "VRS apply";
        ctx.cmd.beginRenderPass(rp);
        ctx.cmd.bindPipeline(applyPipe_.get());
        ctx.cmd.bindUniformBuffer(3, ubo_.get());
        ctx.cmd.bindTexture(0, copy_, ctx.engine.pointClamp());
        ctx.cmd.bindTexture(1, rate_, ctx.engine.pointClamp());
        ctx.cmd.draw(3);
        ctx.cmd.endRenderPass();
    }

    Setting *mode_ = nullptr, *overlay_ = nullptr;
    rhi::Program* rateProg_ = nullptr;
    rhi::PipelinePtr applyPipe_;
    rhi::BufferPtr ubo_;
    rhi::Texture *rate_ = nullptr, *copy_ = nullptr;
    uint32_t tilesX_ = 1, tilesY_ = 1;
};

}  // namespace

GFX_REGISTER_MODULE(VrsModule);
