// geometry.foliage — Grass density, foliage quality, foliage draw distance, wind, interaction.
//
//   GBuffer order 15 "Grass": 4 compute dispatches (distance tiers) place + frustum-cull blades into
//   an SSBO and bump an indirect draw count; then one instanced indirect strip draw.
// Docs: docs/foliage_grass.md
#include <algorithm>
#include <cmath>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

struct GrassParams { glm::vec4 a, b, c, d; glm::vec4 planes[5]; };   // std140, mirrors grass_common.glsl
constexpr int kTiers = 4;
constexpr uint32_t kMaxBlades = 600000;

class FoliageModule : public Module {
public:
    const char* name() const override { return "geometry.foliage"; }

    void registerSettings(SettingsRegistry& s) override {
        density_ = &s.addFloat({.id = "world.grassDensity", .label = "Grass Density", .category = "World", .group = "Foliage",
                                .tooltip = "How many grass blades grow per square metre (0 = none).", .doc = "foliage_grass"},
                               0.75f, 0.0f, 1.0f).presets(0.25f, 0.5f, 0.75f, 1.0f);
        quality_ = &s.addEnum({.id = "world.foliageQuality", .label = "Foliage Quality", .category = "World", .group = "Foliage",
                               .tooltip = "Vertices per blade: 3 / 5 / 7 / 15 (smoother bending).", .doc = "foliage_grass"},
                              {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3);
        distance_ = &s.addFloat({.id = "world.foliageDistance", .label = "Foliage Draw Distance", .category = "World",
                                 .group = "Foliage", .tooltip = "Grass is not generated beyond this distance (metres).",
                                 .doc = "foliage_grass"}, 120.0f, 20.0f, 300.0f).presets(40.0f, 80.0f, 120.0f, 250.0f).fmt("%.0f m");
        wind_ = &s.addBool({.id = "world.foliageWind", .label = "Wind Animation", .category = "World", .group = "Foliage",
                            .tooltip = "Grass sways in the wind.", .doc = "foliage_grass"}, true).presets(0, 1, 1, 1);
        interact_ = &s.addBool({.id = "world.grassInteraction", .label = "Grass Interaction", .category = "World", .group = "Foliage",
                                .tooltip = "Grass bends away from the camera (the 'player') when it walks through.",
                                .doc = "foliage_grass"}, true).presets(0, 0, 1, 1);
    }

    void init(Engine& e) override {
        genProg_ = e.shaders().compute("geometry/grass_gen.comp");
        rhi::PipelineDesc d;
        d.program = e.shaders().graphics("geometry/grass.vert", "geometry/grass.frag");
        d.topology = rhi::Topology::TriangleStrip;
        d.raster.cull = rhi::CullMode::None;
        d.depth = {true, true, rhi::CompareOp::Greater};
        d.name = "grass";
        pipe_ = e.device().createPipeline(d);
        blades_ = geo::makeBuffer(e, size_t(kMaxBlades) * 32, rhi::BufUsageStorage, "grass.blades", name(), nullptr, 32);
        args_ = geo::makeBuffer(e, 16, rhi::BufUsageStorage | rhi::BufUsageIndirect, "grass.args", name());
        for (auto& u : ubo_) u = geo::makeBuffer(e, sizeof(GrassParams), rhi::BufUsageUniform, "grass.params", name());
        auto& hf = geo::terrainHeights();
        rhi::TextureDesc td; td.format = rhi::Format::R32F; td.width = td.height = uint32_t(hf.res);
        td.name = "grass.height"; td.owner = name();
        height_ = e.device().createTexture(td, hf.h.data());
    }

    void addPasses(PassList& p) override {
        p.add(Stage::GBuffer, "Grass", [this](FrameContext& ctx) {
            if (!ctx.settings.getBool("world.showcase") || ctx.msaaForward) return;
            run(ctx);
        }, 15.0f, [this] { return density_->asFloat() > 0.0f; });
    }

private:
    void run(FrameContext& ctx) {
        static const int kSegs[4] = {1, 2, 3, 7};
        int segs = kSegs[std::clamp(quality_->asInt(), 0, 3)];
        float dist = distance_->asFloat();
        float spacing = 0.22f / std::sqrt(std::max(density_->asFloat(), 0.05f));
        Setting* dispS = ctx.settings.find("world.terrainDisplacement");
        float disp = (dispS ? dispS->asFloat() : 1.0f) * 30.0f;
        geo::Frustum fr = geo::extractFrustum(ctx.frame.unjitteredViewProj);
        glm::vec3 cam(ctx.frame.cameraPos);

        uint32_t reset[4] = {uint32_t(2 * segs + 1), 0, 0, 0};
        ctx.cmd.updateBuffer(args_.get(), reset, sizeof(reset));
        ctx.cmd.barrier(rhi::BarrierStorage);
        ctx.cmd.bindComputeProgram(genProg_);
        ctx.cmd.bindStorageBuffer(8, blades_.get());
        ctx.cmd.bindStorageBuffer(9, args_.get());
        ctx.cmd.bindTexture(0, height_.get(), ctx.engine.linearClamp());
        float inner = 0.0f;
        for (int t = 0; t < kTiers; ++t) {
            float outer = dist * std::pow(2.0f, float(t - (kTiers - 1)));     // dist/8, dist/4, dist/2, dist
            float s = spacing * std::pow(2.0f, float(t));
            uint32_t cells = uint32_t(std::ceil(2.0f * outer / s)) + 1;
            glm::vec2 origin = glm::floor((glm::vec2(cam.x, cam.z) - outer) / s) * s;    // snapped to the tier grid
            GrassParams prm;
            prm.a = {s, outer, inner, std::pow(2.0f, float(t) * 0.75f)};
            prm.b = {dist, float(segs), wind_->asBool() ? 1.0f : 0.0f, interact_->asBool() ? 1.0f : 0.0f};
            prm.c = {origin.x, origin.y, float(cells), disp};
            prm.d = {geo::kTerrainCenter.x, geo::kTerrainCenter.z, geo::kTerrainSize, float(kMaxBlades)};
            for (int i = 0; i < 5; ++i) prm.planes[i] = fr.planes[i];
            ctx.cmd.updateBuffer(ubo_[t].get(), prm);
            ctx.cmd.bindUniformBuffer(3, ubo_[t].get());
            ctx.cmd.dispatch((cells * cells + 63) / 64);
            inner = outer;
        }
        ctx.cmd.barrier(rhi::BarrierStorage | rhi::BarrierIndirect | rhi::BarrierVertex);

        geo::beginGBufferPass(ctx, "Grass");
        ctx.cmd.bindPipeline(pipe_.get());
        ctx.cmd.bindUniformBuffer(3, ubo_[kTiers - 1].get());
        ctx.cmd.bindStorageBuffer(8, blades_.get());
        ctx.cmd.drawIndirect(args_.get(), 0, 1, 16);
        ctx.cmd.endRenderPass();
    }

    Setting *density_ = nullptr, *quality_ = nullptr, *distance_ = nullptr, *wind_ = nullptr, *interact_ = nullptr;
    rhi::Program* genProg_ = nullptr;
    rhi::PipelinePtr pipe_;
    rhi::BufferPtr blades_, args_, ubo_[kTiers];
    rhi::TexturePtr height_;
};

}  // namespace

GFX_REGISTER_MODULE(FoliageModule);
