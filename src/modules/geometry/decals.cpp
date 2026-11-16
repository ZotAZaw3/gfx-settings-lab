// geometry.decals — Deferred box decals (scorch marks, bullet holes, paint, puddles) on the plaza.
//
//   GBuffer stage order 50 "Decals": copy gbuffer.normal/material, draw instanced boxes that
//   reconstruct position from scene.depth and blend into albedo / normal / roughness.
// Docs: docs/decals.md
#include <algorithm>
#include <cmath>
#include <random>

#include <glm/gtc/matrix_transform.hpp>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

struct GpuDecal { glm::mat4 model, invModel; glm::vec4 params; };   // std430, mirrors decal_common.glsl
constexpr uint32_t kMaxDecalsCap = 2048;

class DecalsModule : public Module {
public:
    const char* name() const override { return "geometry.decals"; }

    void registerSettings(SettingsRegistry& s) override {
        quality_ = &s.addEnum({.id = "effects.decalQuality", .label = "Decal Quality", .category = "Effects", .group = "Decals",
                               .tooltip = "Which surface channels decals change: colour, + normal, + roughness.",
                               .doc = "decals"}, {"Low", "Medium", "High"}, 2).presets(0, 1, 2, 2);
        max_ = &s.addInt({.id = "effects.maxDecals", .label = "Max Decals", .category = "Effects", .group = "Decals",
                          .tooltip = "How many decals may exist at once (oldest are recycled).", .doc = "decals"},
                         256, 16, int(kMaxDecalsCap)).presets(64, 128, 256, 1024);
        lifetime_ = &s.addFloat({.id = "effects.decalLifetime", .label = "Decal Lifetime", .category = "Effects", .group = "Decals",
                                 .tooltip = "Seconds before a decal fades out.", .doc = "decals"}, 30.0f, 5.0f, 120.0f)
                        .presets(10.0f, 20.0f, 30.0f, 60.0f).fmt("%.0f s");
    }

    void init(Engine& e) override {
        geo::SimpleMesh box = geo::makeBox();
        vb_ = geo::makeBuffer(e, box.vertices.size() * sizeof(geo::SimpleVertex), rhi::BufUsageVertex, "decals.vb", name(), box.vertices.data());
        ib_ = geo::makeBuffer(e, box.indices.size() * 4, rhi::BufUsageIndex, "decals.ib", name(), box.indices.data());
        indexCount_ = uint32_t(box.indices.size());
        ssbo_ = geo::makeBuffer(e, kMaxDecalsCap * sizeof(GpuDecal), rhi::BufUsageStorage, "decals.ssbo", name(), nullptr, sizeof(GpuDecal));
        ubo_ = geo::makeBuffer(e, 16, rhi::BufUsageUniform, "decals.params", name());
        rhi::Program* prog = e.shaders().graphics("geometry/decal.vert", "geometry/decal.frag");
        for (int q = 0; q < 3; ++q) {
            rhi::PipelineDesc d = geo::gbufferPipelineDesc(prog, "decals");
            d.depth = {false, false, rhi::CompareOp::Always};
            d.blend[0] = {rhi::BlendMode::Alpha, 0x7};                 // albedo rgb, keep specular occlusion
            d.blend[1] = {rhi::BlendMode::Opaque, uint8_t(q >= 1 ? 0x7 : 0x0)};   // normal xyz, keep shading model
            d.blend[2] = {rhi::BlendMode::Opaque, uint8_t(q >= 2 ? 0x1 : 0x0)};   // roughness only
            pipes_[q] = e.device().createPipeline(d);
        }
    }

    void resize(Engine& e, Extent, Extent) override {
        rhi::TextureDesc d; d.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget; d.owner = name();
        d.format = rhi::Format::RGBA16F; d.name = "geometry.decalNormalCopy";
        normalCopy_ = e.targets().create(d.name, d, TargetScale::Render);
        d.format = rhi::Format::RGBA8; d.name = "geometry.decalMaterialCopy";
        materialCopy_ = e.targets().create(d.name, d, TargetScale::Render);
    }

    void update(FrameContext& ctx) override {
        uint32_t maxN = uint32_t(std::clamp(max_->asInt(), 16, int(kMaxDecalsCap)));
        float life = lifetime_->asFloat();
        float dt = std::min(ctx.dt, 0.1f);
        if (!primed_) {   // start with a full set at random ages so the effect is visible immediately
            primed_ = true;
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            for (uint32_t i = 0; i < maxN; ++i) { decals_.push_back(makeDecal(life)); decals_.back().params.y = u(rng_) * life; }
        }
        for (auto& d : decals_) d.params.y += dt;
        decals_.erase(std::remove_if(decals_.begin(), decals_.end(), [&](const GpuDecal& d) { return d.params.y > d.params.z; }),
                      decals_.end());
        spawnAccum_ += dt * float(maxN) / life;       // steady state: maxN decals alive
        while (spawnAccum_ >= 1.0f) {
            spawnAccum_ -= 1.0f;
            if (decals_.size() >= maxN) decals_.erase(decals_.begin());   // recycle the oldest
            decals_.push_back(makeDecal(life));
        }
        while (decals_.size() > maxN) decals_.erase(decals_.begin());
    }

    void addPasses(PassList& p) override {
        p.add(Stage::GBuffer, "Decals", [this](FrameContext& ctx) {
            if (!ctx.settings.getBool("world.showcase") || ctx.msaaForward || decals_.empty()) return;
            draw(ctx);
        }, 50.0f);
    }

    size_t cpuMemoryBytes() const override { return decals_.capacity() * sizeof(GpuDecal); }

private:
    GpuDecal makeDecal(float life) {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        float a = u(rng_) * 6.2831853f, r = std::sqrt(u(rng_)) * (geo::kPlazaRadius - 2.0f);
        glm::vec3 p = geo::kPlaza + glm::vec3(std::cos(a) * r, 0.0f, std::sin(a) * r);
        static const int kKinds[8] = {0, 0, 1, 1, 1, 2, 3, 3};   // scorch, bullet holes, paint, puddles
        int kind = kKinds[int(u(rng_) * 8.0f) % 8];
        float size = (kind == 1) ? 0.4f + u(rng_) * 0.3f : 0.8f + u(rng_) * 1.4f;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), p) *
                      glm::rotate(glm::mat4(1.0f), u(rng_) * 6.2831853f, glm::vec3(0, 1, 0)) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(size, 1.5f, size));
        GpuDecal d;
        d.model = m; d.invModel = glm::inverse(m);
        d.params = {float(kind), 0.0f, life, 1.0f};
        return d;
    }

    void draw(FrameContext& ctx) {
        int q = std::clamp(quality_->asInt(), 0, 2);
        ctx.cmd.updateBuffer(ssbo_.get(), decals_.data(), decals_.size() * sizeof(GpuDecal));
        float prm[4] = {float(q), 0, 0, 0};
        ctx.cmd.updateBuffer(ubo_.get(), prm, sizeof(prm));
        ctx.cmd.copyTexture(ctx.targets.get("gbuffer.normal"), normalCopy_);
        ctx.cmd.copyTexture(ctx.targets.get("gbuffer.material"), materialCopy_);

        rhi::RenderPassDesc rp;
        const char* names[3] = {"gbuffer.albedo", "gbuffer.normal", "gbuffer.material"};
        for (int i = 0; i < 3; ++i) { rp.colors[i].texture = ctx.targets.get(names[i]); rp.colors[i].load = rhi::LoadOp::Load; }
        rp.colorCount = 3; rp.name = "Decals";    // no depth attachment: scene.depth is sampled instead
        ctx.cmd.beginRenderPass(rp);
        ctx.cmd.bindPipeline(pipes_[q].get());
        ctx.cmd.bindUniformBuffer(3, ubo_.get());
        ctx.cmd.bindStorageBuffer(8, ssbo_.get());
        ctx.cmd.bindTexture(0, ctx.targets.get("scene.depth"), ctx.engine.pointClamp());
        ctx.cmd.bindTexture(1, normalCopy_, ctx.engine.pointClamp());
        ctx.cmd.bindTexture(2, materialCopy_, ctx.engine.pointClamp());
        ctx.cmd.bindVertexBuffer(0, vb_.get());
        ctx.cmd.bindIndexBuffer(ib_.get(), rhi::IndexType::U32);
        ctx.cmd.drawIndexed(indexCount_, uint32_t(decals_.size()));
        ctx.cmd.endRenderPass();
        ctx.profiler.counter("decals.alive", double(decals_.size()));
    }

    Setting *quality_ = nullptr, *max_ = nullptr, *lifetime_ = nullptr;
    rhi::BufferPtr vb_, ib_, ssbo_, ubo_;
    rhi::PipelinePtr pipes_[3];
    rhi::Texture *normalCopy_ = nullptr, *materialCopy_ = nullptr;
    uint32_t indexCount_ = 0;
    std::vector<GpuDecal> decals_;
    float spawnAccum_ = 0.0f;
    bool primed_ = false;
    std::mt19937 rng_{1234};
};

}  // namespace

GFX_REGISTER_MODULE(DecalsModule);
