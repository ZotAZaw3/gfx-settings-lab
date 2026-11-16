// geometry.particles — GPU particles + Effects (VFX) quality.
//
//   FrameSetup  order 50 : "Particle sim"   compute, one thread per particle, 3 emitters on the plaza
//   Translucent order 20 : "Effects depth"  downsample scene.depth (half/quarter res effects only)
//                          "Particles"      draw into geometry.fx (premultiplied, manual depth test)
//                          "Effects composite" geometry.fx -> scene.color (ONE, ONE_MINUS_SRC_ALPHA)
// Docs: docs/particles.md, docs/vfx_quality.md
#include <algorithm>
#include <cmath>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

struct EmitterParams {   // std140, mirrors shaders/geometry/particle_common.glsl
    glm::vec4 a, b, c, d;
};
struct Particle { glm::vec4 posLife, velAge, color; };

class ParticlesModule : public Module {
public:
    const char* name() const override { return "geometry.particles"; }

    void registerSettings(SettingsRegistry& s) override {
        vfx_ = &s.addEnum({.id = "effects.vfxQuality", .label = "Effects Quality", .category = "Effects", .group = "Effects",
                           .tooltip = "Bundle: particle budget, effects resolution, flipbook blending, spawn rate.",
                           .doc = "vfx_quality"}, {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3)
                   .changed([this](Setting& st) { applyVfxPreset(st.asInt()); });
        quality_ = &s.addEnum({.id = "effects.particleQuality", .label = "Particle Quality", .category = "Effects",
                               .group = "Particles", .tooltip = "Particle budget: 16k / 48k / 128k / 512k GPU particles.",
                               .doc = "particles"}, {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3);
        spawn_ = &s.addFloat({.id = "effects.spawnScale", .label = "Spawn Rate", .category = "Effects", .group = "Particles",
                              .tooltip = "Fraction of the particle budget that is alive.", .doc = "vfx_quality"},
                             1.0f, 0.1f, 1.0f).presets(0.25f, 0.5f, 1.0f, 1.0f);
        lit_ = &s.addEnum({.id = "effects.particleLighting", .label = "Particle Lighting", .category = "Effects",
                           .group = "Particles", .tooltip = "Smoke is lit by the sun and sky instead of a flat colour.",
                           .doc = "particles"}, {"Unlit", "Lit"}, 1).presets(0, 0, 1, 1);
        soft_ = &s.addBool({.id = "effects.softParticles", .label = "Soft Particles", .category = "Effects", .group = "Particles",
                            .tooltip = "Fade particles where they touch geometry (no hard intersection lines).",
                            .doc = "particles"}, true).presets(0, 1, 1, 1);
        collide_ = &s.addBool({.id = "effects.particleCollision", .label = "Particle Collision", .category = "Effects",
                               .group = "Particles", .tooltip = "Sparks bounce off whatever is on screen (depth-buffer collision).",
                               .doc = "particles"}, true).presets(0, 0, 1, 1);
        res_ = &s.addEnum({.id = "effects.resolution", .label = "Effects Resolution", .category = "Effects", .group = "Effects",
                           .tooltip = "Resolution transparent effects are rendered at, then upsampled.",
                           .doc = "vfx_quality", .flags = SettingRequiresResize}, {"Full", "Half", "Quarter"}, 0)
                   .presets(2, 1, 0, 0);
        flip_ = &s.addBool({.id = "effects.flipbookBlend", .label = "Flipbook Interpolation", .category = "Effects",
                            .group = "Effects", .tooltip = "Blend animation frames of smoke/fire sprites for smooth motion.",
                            .doc = "vfx_quality"}, true).presets(0, 1, 1, 1);
    }

    void init(Engine& e) override {
        simProg_ = e.shaders().compute("geometry/particles_sim.comp");
        rhi::PipelineDesc d;
        d.program = e.shaders().graphics("geometry/particles.vert", "geometry/particles.frag");
        d.raster.cull = rhi::CullMode::None;
        d.depth = {false, false, rhi::CompareOp::Always};
        d.blend[0].mode = rhi::BlendMode::Premultiplied;
        d.name = "particles";
        drawPipe_ = e.device().createPipeline(d);
        compositePipe_ = e.createFullscreenPipeline("geometry/fx_composite.frag", {}, rhi::BlendMode::Premultiplied);
        downPipe_ = e.createFullscreenPipeline("geometry/depth_downsample.frag");
        for (auto& b : emitterUBO_) b = geo::makeBuffer(e, sizeof(EmitterParams), rhi::BufUsageUniform, "particles.emitter", name());
        downUBO_ = geo::makeBuffer(e, 16, rhi::BufUsageUniform, "particles.down", name());
        allocate(e);
    }

    void resize(Engine& e, Extent, Extent) override {
        static const float kScale[3] = {1.0f, 0.5f, 0.25f};
        float sc = kScale[std::clamp(res_->asInt(), 0, 2)];
        rhi::TextureDesc fx; fx.format = rhi::Format::RGBA16F; fx.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
        fx.name = "geometry.fx"; fx.owner = name();
        e.targets().remove("geometry.fx");
        e.targets().remove("geometry.fxDepth");
        fxTarget_ = e.targets().create("geometry.fx", fx, TargetScale::Render, sc);
        fxDepth_ = nullptr;
        if (sc < 1.0f) {
            rhi::TextureDesc dd = fx; dd.format = rhi::Format::R32F; dd.name = "geometry.fxDepth";
            fxDepth_ = e.targets().create("geometry.fxDepth", dd, TargetScale::Render, sc);
        }
        factor_ = sc < 1.0f ? int(1.0f / sc + 0.5f) : 1;
    }

    void addPasses(PassList& p) override {
        auto on = [](FrameContext& ctx) { return ctx.settings.getBool("world.showcase"); };
        p.add(Stage::FrameSetup, "Particle sim", [this, on](FrameContext& ctx) { if (on(ctx)) simulate(ctx); }, 50.0f);
        p.add(Stage::Translucent, "Particles", [this, on](FrameContext& ctx) { if (on(ctx)) render(ctx); }, 20.0f);
    }

    void update(FrameContext& ctx) override {
        if (budgetFor(quality_->asInt()) != capacity_) allocate(ctx.engine);
    }

    size_t cpuMemoryBytes() const override { return 0; }

private:
    static uint32_t budgetFor(int q) { static const uint32_t b[4] = {16384, 49152, 131072, 524288}; return b[std::clamp(q, 0, 3)]; }

    void applyVfxPreset(int q) {
        static const struct { int pq; float spawn; int res; bool flip; } k[4] = {
            {0, 0.25f, 2, false}, {1, 0.5f, 1, true}, {2, 1.0f, 0, true}, {3, 1.0f, 0, true}};
        auto& v = k[std::clamp(q, 0, 3)];
        quality_->setNumber(float(v.pq)); spawn_->setNumber(v.spawn);
        res_->setNumber(float(v.res)); flip_->setNumber(v.flip ? 1.0f : 0.0f);
    }

    void allocate(Engine& e) {
        capacity_ = budgetFor(quality_->asInt());
        std::vector<Particle> zero(capacity_, Particle{glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)});
        particles_ = geo::makeBuffer(e, zero.size() * sizeof(Particle), rhi::BufUsageStorage, "particles.ssbo", name(),
                                     zero.data(), sizeof(Particle));
    }

    // emitter i: base, count, type, position
    void emitterParams(FrameContext& ctx, int i, EmitterParams& prm) {
        static const float kShare[3] = {0.3f, 0.3f, 0.4f};
        static const glm::vec3 kPos[3] = {{6.0f, 0.1f, 4.0f}, {10.0f, 0.1f, -2.0f}, {6.0f, 0.1f, -6.0f}};
        uint32_t base = 0;
        for (int k = 0; k < i; ++k) base += uint32_t(capacity_ * kShare[k]);
        uint32_t count = uint32_t(capacity_ * kShare[i] * spawn_->asFloat());
        glm::vec3 pos = geo::kPlaza + kPos[i];
        float sx = 1.0f / float(fxTarget_ ? fxTarget_->width() : ctx.renderSize.width);
        float sy = 1.0f / float(fxTarget_ ? fxTarget_->height() : ctx.renderSize.height);
        prm.a = {pos, float(i)};
        prm.b = {float(count), float(base), std::min(ctx.dt, 0.05f), ctx.time};
        prm.c = {collide_->asBool() ? 1.0f : 0.0f, soft_->asBool() ? 1.0f : 0.0f, 0.6f, float(lit_->asInt())};
        prm.d = {flip_->asBool() ? 1.0f : 0.0f, 1.0f, sx, sy};
        counts_[i] = count;
    }

    void simulate(FrameContext& ctx) {
        // scene.depth here is LAST frame's depth (G-buffer hasn't run yet): fine for collision
        ctx.cmd.bindComputeProgram(simProg_);
        ctx.cmd.bindStorageBuffer(8, particles_.get());
        ctx.cmd.bindTexture(0, ctx.targets.get("scene.depth"), ctx.engine.pointClamp());
        uint32_t alive = 0;
        for (int i = 0; i < 3; ++i) {
            EmitterParams prm; emitterParams(ctx, i, prm);
            ctx.cmd.updateBuffer(emitterUBO_[i].get(), prm);
            ctx.cmd.bindUniformBuffer(3, emitterUBO_[i].get());
            ctx.cmd.dispatch((counts_[i] + 255) / 256);
            alive += counts_[i];
        }
        ctx.cmd.barrier(rhi::BarrierStorage | rhi::BarrierVertex);
        ctx.profiler.counter("particles.alive", alive);
    }

    void render(FrameContext& ctx) {
        rhi::Texture* depth = ctx.targets.get("scene.depth");
        rhi::Texture* testDepth = depth;
        if (fxDepth_) {   // reduced-res effects: downsample depth by max (nearest)
            int f[4] = {factor_, 0, 0, 0};
            ctx.cmd.updateBuffer(downUBO_.get(), f, sizeof(f));
            rhi::RenderPassDesc rp; rp.colors[0].texture = fxDepth_; rp.colors[0].load = rhi::LoadOp::DontCare; rp.colorCount = 1;
            rp.name = "Effects depth";
            ctx.cmd.beginRenderPass(rp);
            ctx.cmd.bindPipeline(downPipe_.get());
            ctx.cmd.bindUniformBuffer(3, downUBO_.get());
            ctx.cmd.bindTexture(0, depth, ctx.engine.pointClamp());
            ctx.cmd.draw(3);
            ctx.cmd.endRenderPass();
            testDepth = fxDepth_;
        }
        rhi::RenderPassDesc rp; rp.colors[0].texture = fxTarget_; rp.colors[0].load = rhi::LoadOp::Clear; rp.colorCount = 1;
        rp.name = "Particles";
        ctx.cmd.beginRenderPass(rp);
        ctx.cmd.bindPipeline(drawPipe_.get());
        ctx.cmd.bindStorageBuffer(8, particles_.get());
        ctx.cmd.bindTexture(0, testDepth, ctx.engine.pointClamp());
        for (int i = 0; i < 3; ++i) {
            ctx.cmd.bindUniformBuffer(3, emitterUBO_[i].get());
            ctx.cmd.draw(counts_[i] * 6);
        }
        ctx.cmd.endRenderPass();

        rhi::RenderPassDesc cp; cp.colors[0].texture = ctx.targets.get("scene.color"); cp.colors[0].load = rhi::LoadOp::Load;
        cp.colorCount = 1; cp.name = "Effects composite";
        ctx.cmd.beginRenderPass(cp);
        ctx.cmd.bindPipeline(compositePipe_.get());
        ctx.cmd.bindTexture(0, fxTarget_, ctx.engine.linearClamp());
        ctx.cmd.draw(3);
        ctx.cmd.endRenderPass();
    }

    Setting *vfx_ = nullptr, *quality_ = nullptr, *spawn_ = nullptr, *lit_ = nullptr, *soft_ = nullptr, *collide_ = nullptr;
    Setting *res_ = nullptr, *flip_ = nullptr;
    rhi::Program* simProg_ = nullptr;
    rhi::PipelinePtr drawPipe_, compositePipe_, downPipe_;
    rhi::BufferPtr particles_, emitterUBO_[3], downUBO_;
    rhi::Texture *fxTarget_ = nullptr, *fxDepth_ = nullptr;
    uint32_t capacity_ = 0, counts_[3] = {};
    int factor_ = 1;
};

}  // namespace

GFX_REGISTER_MODULE(ParticlesModule);
