// geometry.volumetrics — Volumetric fog + volumetric clouds (ray-marched, reduced resolution).
//
//   Translucent order 10: "Volumetrics depth" (downsample, if < full res) -> "Volumetrics" (ray-march)
//                         -> "Volumetrics composite" (depth-aware upsample into scene.color)
// Docs: docs/volumetric_fog_clouds.md
#include <algorithm>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

struct VolumeParams { glm::vec4 a, b, c, d; };   // std140, mirrors VolumeUBO in volumetrics.frag

class VolumetricsModule : public Module {
public:
    const char* name() const override { return "geometry.volumetrics"; }

    void registerSettings(SettingsRegistry& s) override {
        fog_ = &s.addEnum({.id = "effects.volumetricFog", .label = "Volumetric Fog", .category = "Effects", .group = "Atmosphere",
                           .tooltip = "Lit, shadowed fog (light shafts). Off = cheap analytic height fog.",
                           .doc = "volumetric_fog_clouds", .flags = SettingRequiresResize},
                          {"Off", "Low", "Medium", "High", "Ultra"}, 3).presets(1, 2, 3, 4);
        density_ = &s.addFloat({.id = "effects.fogDensity", .label = "Fog Density", .category = "Effects", .group = "Atmosphere",
                                .tooltip = "Extinction per metre at ground level (0 = no fog).", .doc = "volumetric_fog_clouds",
                                .flags = SettingNoPreset}, 0.012f, 0.0f, 0.1f).fmt("%.3f");
        clouds_ = &s.addEnum({.id = "effects.volumetricClouds", .label = "Volumetric Clouds", .category = "Effects",
                              .group = "Atmosphere", .tooltip = "Ray-marched 3D clouds lit by the sun.",
                              .doc = "volumetric_fog_clouds"}, {"Off", "Low", "Medium", "High"}, 2).presets(0, 1, 2, 3);
        coverage_ = &s.addFloat({.id = "effects.cloudCoverage", .label = "Cloud Coverage", .category = "Effects",
                                 .group = "Atmosphere", .tooltip = "How much of the sky is covered.",
                                 .doc = "volumetric_fog_clouds", .flags = SettingNoPreset}, 0.5f, 0.0f, 1.0f)
                        .visible([this] { return clouds_->asInt() > 0; });
    }

    void init(Engine& e) override {
        marchPipe_ = e.createFullscreenPipeline("geometry/volumetrics.frag");
        compositePipe_ = e.createFullscreenPipeline("geometry/fog_composite.frag");
        downPipe_ = e.createFullscreenPipeline("geometry/depth_downsample.frag");
        params_ = geo::makeBuffer(e, sizeof(VolumeParams), rhi::BufUsageUniform, "volumetrics.params", name());
        downUBO_ = geo::makeBuffer(e, 16, rhi::BufUsageUniform, "volumetrics.down", name());
        auto& hf = geo::terrainHeights();
        rhi::TextureDesc td; td.format = rhi::Format::R32F; td.width = td.height = uint32_t(hf.res);
        td.name = "volumetrics.height"; td.owner = name();
        height_ = e.device().createTexture(td, hf.h.data());
    }

    void resize(Engine& e, Extent, Extent) override {
        static const float kScale[5] = {0.25f, 0.25f, 0.5f, 0.5f, 1.0f};
        float sc = kScale[std::clamp(fog_->asInt(), 0, 4)];
        factor_ = int(1.0f / sc + 0.5f);
        for (const char* n : {"geometry.vol", "geometry.volDepth", "geometry.colorCopy"}) e.targets().remove(n);
        rhi::TextureDesc d; d.format = rhi::Format::RGBA16F; d.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
        d.owner = name(); d.name = "geometry.vol";
        vol_ = e.targets().create("geometry.vol", d, TargetScale::Render, sc);
        d.name = "geometry.colorCopy";
        colorCopy_ = e.targets().create("geometry.colorCopy", d, TargetScale::Render, 1.0f);
        volDepth_ = nullptr;
        if (factor_ > 1) {
            d.format = rhi::Format::R32F; d.name = "geometry.volDepth";
            volDepth_ = e.targets().create("geometry.volDepth", d, TargetScale::Render, sc);
        }
    }

    void addPasses(PassList& p) override {
        p.add(Stage::Translucent, "Volumetrics", [this](FrameContext& ctx) { run(ctx); }, 10.0f,
              [this] { return density_->asFloat() > 0.0f || clouds_->asInt() > 0; });
    }

private:
    void run(FrameContext& ctx) {
        static const float kSteps[5] = {0, 16, 32, 48, 96};
        static const float kShadow[5] = {0, 0, 1, 1, 1};
        static const float kCloudSteps[4] = {0, 32, 64, 128}, kCloudLight[4] = {0, 3, 4, 6};
        int f = std::clamp(fog_->asInt(), 0, 4), c = std::clamp(clouds_->asInt(), 0, 3);
        Setting* dispS = ctx.settings.find("world.terrainDisplacement");
        bool showcase = ctx.settings.getBool("world.showcase");
        VolumeParams prm;
        prm.a = {kSteps[f], density_->asFloat(), 0.06f, 400.0f};
        prm.b = {0.6f, showcase ? kShadow[f] : 0.0f, kCloudSteps[c], kCloudLight[c]};
        prm.c = {coverage_->asFloat(), 1500.0f, 3500.0f, (dispS ? dispS->asFloat() : 1.0f) * 30.0f};
        prm.d = {geo::kTerrainCenter.x, geo::kTerrainCenter.z, geo::kTerrainSize, 0.0f};
        ctx.cmd.updateBuffer(params_.get(), prm);

        rhi::Texture* depth = ctx.targets.get("scene.depth");
        rhi::Texture* marchDepth = depth;
        if (volDepth_) {
            int fac[4] = {factor_, 0, 0, 0};
            ctx.cmd.updateBuffer(downUBO_.get(), fac, sizeof(fac));
            fullscreenInto(ctx, volDepth_, downPipe_.get(), "Volumetrics depth", [&] {
                ctx.cmd.bindUniformBuffer(3, downUBO_.get());
                ctx.cmd.bindTexture(0, depth, ctx.engine.pointClamp());
            });
            marchDepth = volDepth_;
        }
        fullscreenInto(ctx, vol_, marchPipe_.get(), "Volumetrics march", [&] {
            ctx.cmd.bindUniformBuffer(3, params_.get());
            ctx.cmd.bindTexture(0, marchDepth, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(1, height_.get(), ctx.engine.linearClamp());
        });
        rhi::Texture* color = ctx.targets.get("scene.color");
        ctx.cmd.copyTexture(color, colorCopy_);
        fullscreenInto(ctx, color, compositePipe_.get(), "Volumetrics composite", [&] {
            ctx.cmd.bindTexture(0, vol_, ctx.engine.linearClamp());
            ctx.cmd.bindTexture(1, depth, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(2, marchDepth, ctx.engine.pointClamp());
            ctx.cmd.bindTexture(3, colorCopy_, ctx.engine.pointClamp());
        });
    }

    template <class Bind>
    void fullscreenInto(FrameContext& ctx, rhi::Texture* target, rhi::Pipeline* pipe, const char* label, Bind bind) {
        rhi::RenderPassDesc rp; rp.colors[0].texture = target; rp.colors[0].load = rhi::LoadOp::DontCare; rp.colorCount = 1;
        rp.name = label;
        ctx.cmd.beginRenderPass(rp);
        ctx.cmd.bindPipeline(pipe);
        bind();
        ctx.cmd.draw(3);
        ctx.cmd.endRenderPass();
    }

    Setting *fog_ = nullptr, *density_ = nullptr, *clouds_ = nullptr, *coverage_ = nullptr;
    rhi::PipelinePtr marchPipe_, compositePipe_, downPipe_;
    rhi::BufferPtr params_, downUBO_;
    rhi::TexturePtr height_;
    rhi::Texture *vol_ = nullptr, *volDepth_ = nullptr, *colorCopy_ = nullptr;
    int factor_ = 1;
};

}  // namespace

GFX_REGISTER_MODULE(VolumetricsModule);
