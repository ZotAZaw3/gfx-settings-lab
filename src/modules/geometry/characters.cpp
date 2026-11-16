// geometry.characters — Fur (shell rendering), Cloth simulation, Crowd density. All on the plaza.
//
//   FrameSetup order 60  "Cloth sim"   Verlet integrate + N Jacobi constraint iterations (compute)
//   GBuffer    order 20  "Characters"  fur ball (N shells, instanced), cloth flag, instanced crowd
// Docs: docs/hair_fur.md, docs/cloth_physics.md, docs/crowd_density.md
#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

struct CharParams { glm::mat4 model; glm::vec4 a, b, c, d; };   // std140, mirrors char_params.glsl

class CharactersModule : public Module {
public:
    const char* name() const override { return "geometry.characters"; }

    void registerSettings(SettingsRegistry& s) override {
        fur_ = &s.addEnum({.id = "characters.furQuality", .label = "Fur Quality", .category = "Characters", .group = "Hair & Fur",
                           .tooltip = "Shell fur: 0 / 8 / 16 / 32 layers drawn around the mesh.", .doc = "hair_fur"},
                          {"Off", "Low", "Medium", "High"}, 2).presets(0, 1, 2, 3);
        cloth_ = &s.addEnum({.id = "physics.cloth", .label = "Cloth Simulation", .category = "Physics", .group = "Cloth",
                             .tooltip = "Off = canned animation; Low/High = GPU position-based cloth (grid size, iterations).",
                             .doc = "cloth_physics"}, {"Off", "Low", "High"}, 2).presets(0, 1, 2, 2);
        wind_ = &s.addFloat({.id = "physics.windStrength", .label = "Wind Strength", .category = "Physics", .group = "Cloth",
                             .tooltip = "Wind force on cloth.", .doc = "cloth_physics", .flags = SettingNoPreset}, 6.0f, 0.0f, 20.0f);
        crowd_ = &s.addEnum({.id = "world.crowdDensity", .label = "Crowd Density", .category = "World", .group = "Population",
                             .tooltip = "Pedestrians on the plaza: 25 / 75 / 200 / 500.", .doc = "crowd_density"},
                            {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3);
    }

    void init(Engine& e) override {
        auto upload = [&](const geo::SimpleMesh& m, rhi::BufferPtr& vb, rhi::BufferPtr& ib, uint32_t& count, const char* n) {
            vb = geo::makeBuffer(e, m.vertices.size() * sizeof(geo::SimpleVertex), rhi::BufUsageVertex, n, name(), m.vertices.data());
            ib = geo::makeBuffer(e, m.indices.size() * 4, rhi::BufUsageIndex, n, name(), m.indices.data());
            count = uint32_t(m.indices.size());
        };
        upload(geo::makeSphere(48, 32, 1.0f), sphereVB_, sphereIB_, sphereCount_, "fur.sphere");
        upload(geo::makeCapsule(12, 0.25f, 1.2f), capsuleVB_, capsuleIB_, capsuleCount_, "crowd.capsule");
        furPipe_ = e.device().createPipeline(geo::gbufferPipelineDesc(e.shaders().graphics("geometry/fur.vert", "geometry/fur.frag"), "fur"));
        crowdPipe_ = e.device().createPipeline(geo::gbufferPipelineDesc(
            e.shaders().graphics("geometry/crowd.vert", "geometry/simple_gbuffer.frag"), "crowd"));
        rhi::PipelineDesc cd = geo::gbufferPipelineDesc(e.shaders().graphics("geometry/cloth.vert", "geometry/simple_gbuffer.frag"), "cloth");
        cd.layout = {};   // vertex pulling from the simulation SSBO
        clothPipe_ = e.device().createPipeline(cd);
        clothProg_ = e.shaders().compute("geometry/cloth_sim.comp");
        for (auto& u : ubo_) u = geo::makeBuffer(e, sizeof(CharParams), rhi::BufUsageUniform, "characters.params", name());
        buildCloth(e);
    }

    void update(FrameContext& ctx) override {
        if (clothGrid(cloth_->asInt()) != clothW_) buildCloth(ctx.engine);
    }

    void addPasses(PassList& p) override {
        auto on = [](FrameContext& ctx) { return ctx.settings.getBool("world.showcase"); };
        p.add(Stage::FrameSetup, "Cloth sim", [this, on](FrameContext& ctx) { if (on(ctx)) simulateCloth(ctx); }, 60.0f,
              [this] { return cloth_->asInt() > 0; });
        p.add(Stage::GBuffer, "Characters", [this, on](FrameContext& ctx) {
            if (on(ctx) && !ctx.msaaForward) draw(ctx);
        }, 20.0f);
    }

private:
    static int clothGrid(int q) { static const int w[3] = {24, 16, 48}; return w[std::clamp(q, 0, 2)]; }   // Off uses 24 (static)

    void buildCloth(Engine& e) {
        clothW_ = clothGrid(cloth_->asInt());
        float width = 3.0f, height = 2.0f, rest = width / float(clothW_ - 1);
        clothH_ = int(std::round(height / rest)) + 1;
        clothRest_ = rest;
        glm::vec3 pole = geo::kPlaza + glm::vec3(-10.0f, 5.0f, 8.0f);
        std::vector<glm::vec4> pts;
        for (int y = 0; y < clothH_; ++y)
            for (int x = 0; x < clothW_; ++x)
                pts.emplace_back(pole + glm::vec3(x * rest, -y * rest, 0.0f), x == 0 ? 0.0f : 1.0f);   // pinned to the pole
        size_t bytes = pts.size() * sizeof(glm::vec4);
        for (auto& b : clothPos_) b = geo::makeBuffer(e, bytes, rhi::BufUsageStorage, "cloth.pos", name(), pts.data(), 16);
        clothPrev_ = geo::makeBuffer(e, bytes, rhi::BufUsageStorage, "cloth.prev", name(), pts.data(), 16);
        std::vector<uint32_t> idx;
        for (int y = 0; y + 1 < clothH_; ++y)
            for (int x = 0; x + 1 < clothW_; ++x) {
                uint32_t a = y * clothW_ + x, b = a + clothW_;
                idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        clothIndexCount_ = uint32_t(idx.size());
        cur_ = 0;
    }

    void fillCloth(CharParams& p, float mode, float dt) {
        p.model = glm::mat4(1.0f);
        p.a = {float(clothW_), float(clothH_), cloth_->asInt() == 0 ? 1.0f : 0.0f, 0.0f};
        p.b = {0.7f, 0.1f, 0.1f, 0.8f};
        p.c = {dt, clothRest_, mode, wind_->asFloat()};
        p.d = {0.0f, 0.0f, 0.0f, 0.0f};
    }

    void simulateCloth(FrameContext& ctx) {
        static const int kIters[3] = {0, 4, 12};
        int iters = kIters[std::clamp(cloth_->asInt(), 0, 2)];
        uint32_t n = uint32_t(clothW_ * clothH_), groups = (n + 63) / 64;
        float dt = std::clamp(ctx.dt, 1.0f / 240.0f, 1.0f / 30.0f);
        CharParams p0, p1;
        fillCloth(p0, 0.0f, dt); fillCloth(p1, 1.0f, dt);
        ctx.cmd.updateBuffer(ubo_[2].get(), p0);
        ctx.cmd.updateBuffer(ubo_[3].get(), p1);
        ctx.cmd.bindComputeProgram(clothProg_);
        ctx.cmd.bindStorageBuffer(10, clothPrev_.get());
        auto step = [&](rhi::Buffer* ubo) {
            ctx.cmd.bindUniformBuffer(3, ubo);
            ctx.cmd.bindStorageBuffer(8, clothPos_[cur_].get());
            ctx.cmd.bindStorageBuffer(9, clothPos_[cur_ ^ 1].get());
            ctx.cmd.dispatch(groups);
            ctx.cmd.barrier(rhi::BarrierStorage);
            cur_ ^= 1;
        };
        step(ubo_[2].get());
        for (int i = 0; i < iters; ++i) step(ubo_[3].get());
        ctx.cmd.barrier(rhi::BarrierStorage | rhi::BarrierVertex);
        ctx.profiler.counter("cloth.particles", n);
    }

    void draw(FrameContext& ctx) {
        static const int kShells[4] = {0, 8, 16, 32};
        static const uint32_t kCrowd[4] = {25, 75, 200, 500};
        int shells = kShells[std::clamp(fur_->asInt(), 0, 3)];
        uint32_t crowd = kCrowd[std::clamp(crowd_->asInt(), 0, 3)];

        CharParams fur;
        fur.model = glm::translate(glm::mat4(1.0f), geo::kPlaza + glm::vec3(-8.0f, 1.3f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(1.2f));
        fur.a = {float(shells), 0.18f, 70.0f, 0.5f};
        fur.b = {0.55f, 0.38f, 0.22f, 0.7f};
        ctx.cmd.updateBuffer(ubo_[0].get(), fur);
        CharParams cr{};
        cr.model = glm::mat4(1.0f);
        cr.a = {float(crowd), geo::kPlaza.x, geo::kPlaza.z, 0.0f};
        cr.b = {1, 1, 1, 0.6f};
        ctx.cmd.updateBuffer(ubo_[1].get(), cr);
        CharParams cl; fillCloth(cl, 0.0f, 0.0f);
        ctx.cmd.updateBuffer(ubo_[2].get(), cl);

        geo::beginGBufferPass(ctx, "Characters");
        // fur: shell 0 is the skin, 1..N the fur layers (one instanced draw)
        ctx.cmd.bindPipeline(furPipe_.get());
        ctx.cmd.bindUniformBuffer(3, ubo_[0].get());
        ctx.cmd.bindVertexBuffer(0, sphereVB_.get());
        ctx.cmd.bindIndexBuffer(sphereIB_.get(), rhi::IndexType::U32);
        ctx.cmd.drawIndexed(sphereCount_, uint32_t(shells + 1));
        // crowd
        ctx.cmd.bindPipeline(crowdPipe_.get());
        ctx.cmd.bindUniformBuffer(3, ubo_[1].get());
        ctx.cmd.bindVertexBuffer(0, capsuleVB_.get());
        ctx.cmd.bindIndexBuffer(capsuleIB_.get(), rhi::IndexType::U32);
        ctx.cmd.drawIndexed(capsuleCount_, crowd);
        // cloth (vertex pulling)
        ctx.cmd.bindPipeline(clothPipe_.get());
        ctx.cmd.bindUniformBuffer(3, ubo_[2].get());
        ctx.cmd.bindStorageBuffer(8, clothPos_[cur_].get());
        ctx.cmd.draw(clothIndexCount_);   // 6 verts per quad, positions pulled from the SSBO
        ctx.cmd.endRenderPass();
        ctx.profiler.counter("crowd.agents", crowd);
    }

    Setting *fur_ = nullptr, *cloth_ = nullptr, *wind_ = nullptr, *crowd_ = nullptr;
    rhi::BufferPtr sphereVB_, sphereIB_, capsuleVB_, capsuleIB_, clothPos_[2], clothPrev_, ubo_[4];
    uint32_t sphereCount_ = 0, capsuleCount_ = 0, clothIndexCount_ = 0;
    rhi::PipelinePtr furPipe_, crowdPipe_, clothPipe_;
    rhi::Program* clothProg_ = nullptr;
    int clothW_ = 0, clothH_ = 0, cur_ = 0;
    float clothRest_ = 0.1f;
};

}  // namespace

GFX_REGISTER_MODULE(CharactersModule);
