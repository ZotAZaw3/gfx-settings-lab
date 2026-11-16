// geometry.water — Water quality: Gerstner waves, refraction + absorption, SSR, foam, caustics.
//
//   Translucent order 5 "Water": copy scene.color, draw the lake grid with a manual depth test
//   (scene.depth is sampled, not attached) and write the shaded result into scene.color.
// Docs: docs/water_quality.md
#include <algorithm>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

struct WaterParams { glm::vec4 a, b, c; };   // std140, mirrors water_common.glsl

class WaterModule : public Module {
public:
    const char* name() const override { return "geometry.water"; }

    void registerSettings(SettingsRegistry& s) override {
        quality_ = &s.addEnum({.id = "world.waterQuality", .label = "Water Quality", .category = "World", .group = "Water",
                               .tooltip = "Waves (0/4/8/12 Gerstner), mesh density, refraction, reflections, foam.",
                               .doc = "water_quality"}, {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3);
        caustics_ = &s.addBool({.id = "world.caustics", .label = "Caustics", .category = "World", .group = "Water",
                                .tooltip = "Animated light patterns on the lake bottom.", .doc = "water_quality"}, true)
                        .presets(0, 0, 1, 1);
    }

    void init(Engine& e) override {
        static const int kRes[4] = {32, 64, 128, 192};
        for (int q = 0; q < 4; ++q) {
            geo::SimpleMesh g = geo::makeGridXZ(kRes[q], kSize);
            std::vector<glm::vec3> pos; for (auto& v : g.vertices) pos.push_back(v.pos);
            vb_[q] = geo::makeBuffer(e, pos.size() * 12, rhi::BufUsageVertex, "water.vb", name(), pos.data());
            ib_[q] = geo::makeBuffer(e, g.indices.size() * 4, rhi::BufUsageIndex, "water.ib", name(), g.indices.data());
            count_[q] = uint32_t(g.indices.size());
        }
        rhi::PipelineDesc d;
        d.program = e.shaders().graphics("geometry/water.vert", "geometry/water.frag");
        d.layout.attribs = {{0, rhi::VertexFormat::Float3, 0}}; d.layout.strides[0] = 12;
        d.raster.cull = rhi::CullMode::None;
        d.depth = {false, false, rhi::CompareOp::Always};
        d.name = "water";
        pipe_ = e.device().createPipeline(d);
        ubo_ = geo::makeBuffer(e, sizeof(WaterParams), rhi::BufUsageUniform, "water.params", name());
    }

    void resize(Engine& e, Extent, Extent) override {
        rhi::TextureDesc c; c.format = rhi::Format::RGBA16F; c.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
        c.name = "geometry.waterColorCopy"; c.owner = name();
        copy_ = e.targets().create(c.name, c, TargetScale::Render);
    }

    void addPasses(PassList& p) override {
        p.add(Stage::Translucent, "Water", [this](FrameContext& ctx) {
            if (ctx.settings.getBool("world.showcase")) draw(ctx);
        }, 5.0f);
    }

private:
    static constexpr float kSize = 64.0f;

    void draw(FrameContext& ctx) {
        static const float kWaves[4] = {0, 4, 8, 12}, kRefr[4] = {0, 1, 1, 1}, kSsr[4] = {0, 0, 1, 1}, kFoam[4] = {0, 1, 1, 1};
        int q = std::clamp(quality_->asInt(), 0, 3);
        WaterParams prm;
        prm.a = {kWaves[q], kRefr[q], kSsr[q], kFoam[q]};
        prm.b = {caustics_->asBool() ? 1.0f : 0.0f, geo::kWaterLevel, kSize, 0.0f};
        prm.c = {geo::kLake.x, geo::kLake.z, 0.0f, 0.0f};
        ctx.cmd.updateBuffer(ubo_.get(), prm);
        rhi::Texture* color = ctx.targets.get("scene.color");
        ctx.cmd.copyTexture(color, copy_);
        rhi::RenderPassDesc rp; rp.colors[0].texture = color; rp.colors[0].load = rhi::LoadOp::Load; rp.colorCount = 1;
        rp.name = "Water";
        ctx.cmd.beginRenderPass(rp);
        ctx.cmd.bindPipeline(pipe_.get());
        ctx.cmd.bindUniformBuffer(3, ubo_.get());
        ctx.cmd.bindTexture(0, ctx.targets.get("scene.depth"), ctx.engine.pointClamp());
        ctx.cmd.bindTexture(1, copy_, ctx.engine.linearClamp());
        ctx.cmd.bindVertexBuffer(0, vb_[q].get());
        ctx.cmd.bindIndexBuffer(ib_[q].get(), rhi::IndexType::U32);
        ctx.cmd.drawIndexed(count_[q]);
        ctx.cmd.endRenderPass();
    }

    Setting *quality_ = nullptr, *caustics_ = nullptr;
    rhi::BufferPtr vb_[4], ib_[4], ubo_;
    uint32_t count_[4] = {};
    rhi::PipelinePtr pipe_;
    rhi::Texture* copy_ = nullptr;
};

}  // namespace

GFX_REGISTER_MODULE(WaterModule);
