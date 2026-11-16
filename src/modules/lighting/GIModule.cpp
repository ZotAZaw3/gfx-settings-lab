// GIModule ("lighting.gi") - indirect diffuse lighting:
//   flat ambient / hemisphere + IBL / irradiance probe grid (L1 or L2 spherical harmonics, baked
//   progressively one probe per frame) and screen-space GI into the shared lighting.indirectDiffuse.
// Docs: docs/global_illumination.md, docs/reflection_probe_quality.md, docs/emissive_lighting.md
#include <algorithm>

#include "core/engine.h"
#include "modules/lighting/LightingShared.h"

namespace gfx::lighting {

class GIModule : public Module {
public:
    const char* name() const override { return "lighting.gi"; }

    void registerSettings(SettingsRegistry& s) override {
        const char* cat = "Lighting";
        technique_ = &s.addEnum({.id = "lighting.gi.technique", .label = "Global Illumination", .category = cat,
                                 .group = "Global Illumination",
                                 .tooltip = "How bounced light is approximated: flat ambient, sky hemisphere, or a grid of baked light probes.",
                                 .doc = "global_illumination"}, {"Flat ambient", "Hemisphere + IBL", "Light probes (L1 SH)", "Light probes (L2 SH)"}, 3)
                         .presets(1, 2, 3, 3);
        density_ = &s.addEnum({.id = "lighting.gi.probeDensity", .label = "Light Probe Density", .category = cat,
                               .group = "Global Illumination", .tooltip = "Probes in the grid: finer grids follow walls and doorways better.",
                               .doc = "reflection_probe_quality"}, {"4x2x4", "8x4x8", "12x6x12"}, 1)
                       .presets(0, 0, 1, 2)
                       .visible([this] { return technique_->asInt() >= 2; });
        ssgi_ = &s.addEnum({.id = "lighting.gi.ssgi", .label = "Screen Space GI", .category = cat, .group = "Global Illumination",
                            .tooltip = "One bounce of on-screen light (color bleeding, emissive glow on nearby surfaces).",
                            .doc = "global_illumination"}, {"Off", "Low", "Medium", "High", "Ultra"}, 2)
                    .presets(0, 0, 2, 4);
        ssgiIntensity_ = &s.addFloat({.id = "lighting.gi.ssgiIntensity", .label = "SSGI Intensity", .category = cat,
                                      .group = "Global Illumination", .tooltip = "Strength of the screen-space bounce.",
                                      .doc = "global_illumination", .flags = SettingNoPreset}, 1.0f, 0.0f, 4.0f)
                             .visible([this] { return ssgi_->asInt() != 0; });
    }

    void init(Engine& e) override {
        auto& sh = e.shaders();
        ssgiProg_ = sh.compute("lighting/ssgi.comp");
        temporal_ = sh.compute("lighting/temporal_accum.comp");
        shProg_ = sh.compute("lighting/probe_sh_project.comp");
        params_ = e.device().createBuffer({.size = 32, .usage = rhi::BufUsageUniform, .name = "gi.params", .owner = name()});
        fwdParams_ = e.device().createBuffer({.size = 32, .usage = rhi::BufUsageUniform, .name = "gi.fwdParams", .owner = name()});
        rhi::PipelineDesc pd;
        pd.program = sh.graphics("lighting/forward_lite.vert", "lighting/forward_lite.frag");
        pd.layout = SceneRenderer::vertexLayout();
        pd.raster.cull = rhi::CullMode::None;
        pd.name = "lighting.probeCapture";
        capture_ = e.device().createPipeline(pd);
        rhi::TextureDesc c;
        c.type = rhi::TextureType::TexCube;
        c.format = rhi::Format::RGBA16F;
        c.width = c.height = kCaptureRes;
        c.depthOrLayers = 6;
        c.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
        c.name = "lighting.probeCapture";
        c.owner = name();
        cube_ = e.device().createTexture(c);
        rhi::TextureDesc z;
        z.format = rhi::Format::D32F;
        z.width = z.height = kCaptureRes;
        z.usage = rhi::TexUsageDepthStencil;
        z.name = "lighting.probeCaptureDepth";
        z.owner = name();
        cubeDepth_ = e.device().createTexture(z);
    }

    void update(FrameContext& ctx) override {
        auto& S = shared();
        auto& u = S.ubo;
        int tech = technique_->asInt();
        bool rtGI = settingOn(ctx, "rt.gi");

        // ---- probe grid ----
        static const glm::ivec3 dims[3] = {{4, 2, 4}, {8, 4, 8}, {12, 6, 12}};
        glm::ivec3 dim = dims[density_->asInt()];
        bool wantProbes = tech >= 2 && !ctx.replaceLighting;
        glm::vec4 sun(ctx.scene.sun.direction * ctx.scene.sun.intensity, 0.0f);
        if (wantProbes) {
            size_t bytes = sizeof(glm::vec4) * 7 * (size_t)(dim.x * dim.y * dim.z);
            if (!probes_ || probes_->size() != bytes) {
                probes_ = ctx.device.createBuffer({.size = bytes, .usage = rhi::BufUsageStorage, .dynamic = false,
                                                   .structStride = 16, .name = "lighting.probeSH", .owner = name()});
                nextProbe_ = 0;
                baked_ = false;
            }
            if (sun != lastSun_ || ctx.scene.geometryVersion() != geomVersion_) { nextProbe_ = 0; baked_ = false; }
            lastSun_ = sun;
            geomVersion_ = ctx.scene.geometryVersion();
        } else {
            probes_.reset();
            baked_ = false;
        }
        S.probeSSBO = probes_.get();
        dim_ = dim;
        const AABB& b = ctx.scene.bounds;
        glm::vec3 mn = b.valid() ? b.min : glm::vec3(-10, 0, -10), mx = b.valid() ? b.max : glm::vec3(10, 5, 10);
        mx.y = std::max(mx.y, mn.y + 1.0f);
        gridMin_ = mn;
        gridSize_ = mx - mn;
        u.probeGridMin = glm::vec4(mn, 0);
        u.probeGridSize = glm::vec4(gridSize_, 0);
        u.probeGridDim = glm::ivec4(dim, baked_ ? 1 : 0);

        // ---- SSGI ----
        int q = ssgi_->asInt();
        ssgiActive_ = q > 0 && !ctx.replaceLighting && !ctx.msaaForward && !rtGI;
        static const int rays[5] = {0, 2, 4, 6, 8};
        static const int steps[5] = {0, 8, 10, 12, 16};
        u.giCfg = glm::ivec4(tech, ssgiActive_ ? 1 : 0, rays[q], steps[q]);
        u.giParams = glm::vec4(ssgiIntensity_->asFloat(), 3.0f, 0.3f, 0.1f);
        S.ssgiNeedsHiZ = ssgiActive_;
        float scale = q <= 3 ? 0.5f : 1.0f;
        if (ssgiActive_ && (!out_ || scale != scale_)) {
            if (out_) release(ctx);
            scale_ = scale;
            auto d = storageDesc(rhi::Format::RGBA16F, "lighting.indirectDiffuse", name());
            out_ = ctx.targets.create("lighting.indirectDiffuse", d, TargetScale::Render, scale);
            d.name = "lighting.ssgiRaw";
            raw_ = ctx.targets.create("lighting.ssgiRaw", d, TargetScale::Render, scale);
            d.name = "lighting.ssgiHistory";
            hist_ = ctx.targets.create("lighting.ssgiHistory", d, TargetScale::Render, scale);
            historyValid_ = false;
        } else if (!ssgiActive_ && out_) {
            release(ctx);
        }
        if (out_) {
            out_ = ctx.targets.get("lighting.indirectDiffuse");
            raw_ = ctx.targets.get("lighting.ssgiRaw");
            hist_ = ctx.targets.get("lighting.ssgiHistory");
            if (out_ && out_->width() != lastW_) { historyValid_ = false; lastW_ = out_->width(); }
        }
    }

    void addPasses(PassList& p) override {
        // after the environment probe (Shadow order 50) so captures see the sky
        p.add(Stage::Shadow, "Light probe bake", [this](FrameContext& ctx) { bakeProbe(ctx); }, 70.0f,
              [this] { return probes_ != nullptr; });
        p.add(Stage::Lighting, "SSGI", [this](FrameContext& ctx) { runSSGI(ctx); }, 25.0f,
              [this] { return ssgiActive_ && out_ != nullptr; });
    }

private:
    static constexpr uint32_t kCaptureRes = 32;

    // One probe per frame: 6 cube faces with forward_lite, then SH projection into the SSBO.
    // Once the whole grid is baked it keeps refreshing (multi-bounce converges over passes).
    void bakeProbe(FrameContext& ctx) {
        if (ctx.replaceLighting) return;
        int total = dim_.x * dim_.y * dim_.z;
        int i = nextProbe_ % total;
        glm::ivec3 p(i % dim_.x, (i / dim_.x) % dim_.y, i / (dim_.x * dim_.y));
        glm::vec3 t = glm::vec3(p) / glm::max(glm::vec3(dim_ - 1), glm::vec3(1));
        glm::vec3 pos = gridMin_ + t * gridSize_;
        pos.y = std::max(pos.y, gridMin_.y + 0.25f);

        struct { glm::vec4 plane, eye; } fp{glm::vec4(0, 0, 0, 1), glm::vec4(pos, 1)};
        for (int f = 0; f < 6; ++f) {
            rhi::RenderPassDesc rp;
            rp.colors[0].texture = cube_.get();
            rp.colors[0].layer = (uint32_t)f;
            rp.colors[0].load = rhi::LoadOp::Clear;
            // clear to the sky ambient so misses contribute sky light
            glm::vec4 amb = shared().ubo.ambient;
            rp.colors[0].clear[0] = amb.r; rp.colors[0].clear[1] = amb.g; rp.colors[0].clear[2] = amb.b; rp.colors[0].clear[3] = 1;
            rp.colorCount = 1;
            rp.depth.texture = cubeDepth_.get();
            rp.depth.load = rhi::LoadOp::Clear;
            rp.depth.clearDepth = 0.0f;
            rp.name = "Probe capture face";
            ctx.cmd.beginRenderPass(rp);
            ctx.cmd.updateBuffer(fwdParams_.get(), fp);
            ctx.cmd.bindUniformBuffer(3, fwdParams_.get());
            bindShading(ctx, true);
            SceneDrawParams dp;
            dp.mode = DrawMode::Custom;
            dp.viewProj = cubeFaceViewProj(f, pos, 0.05f);
            dp.prevViewProj = dp.viewProj;
            dp.pipeline = capture_.get();
            ctx.renderer.drawScene(ctx, dp);
            ctx.cmd.endRenderPass();
        }
        glm::ivec4 pi(i, 0, 0, 0);
        ctx.cmd.updateBuffer(params_.get(), pi);
        ctx.cmd.bindUniformBuffer(3, params_.get());
        ctx.cmd.bindComputeProgram(shProg_);
        ctx.cmd.bindTexture(6, cube_.get(), ctx.engine.linearClamp());
        ctx.cmd.bindStorageBuffer(4, probes_.get());
        ctx.cmd.dispatch(1);
        ctx.cmd.barrier(rhi::BarrierStorage);
        ++nextProbe_;
        if (nextProbe_ >= total) baked_ = true;
    }

    void release(FrameContext& ctx) {
        for (const char* n : {"lighting.indirectDiffuse", "lighting.ssgiRaw", "lighting.ssgiHistory"}) ctx.targets.remove(n);
        out_ = raw_ = hist_ = nullptr;
    }

    void runSSGI(FrameContext& ctx) {
        if (ctx.replaceLighting || !shared().prevColorValid) {
            float zero[4] = {0, 0, 0, 0};
            if (out_) ctx.cmd.clearTexture(out_, zero);
            return;
        }
        auto& cmd = ctx.cmd;
        rhi::Sampler* pt = ctx.engine.pointClamp();
        glm::vec2 size(out_->width(), out_->height());
        glm::vec4 sp(size, 0, 0);
        cmd.updateBuffer(params_.get(), sp);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindComputeProgram(ssgiProg_);
        cmd.bindTexture(1, ctx.targets.get("gbuffer.normal"), pt);
        cmd.bindTexture(3, ctx.targets.get("scene.depth"), pt);
        cmd.bindTexture(5, shared().prevColor, ctx.engine.linearClamp());
        cmd.bindTexture(7, shared().hiz, pt);
        cmd.bindImage(0, raw_, 0, rhi::Access::Write);
        cmd.dispatch(groups(raw_->width(), 8), groups(raw_->height(), 8));
        cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);

        glm::vec4 ta(size, shared().ubo.giParams.w, historyValid_ ? 0.0f : 1.0f);
        cmd.updateBuffer(params_.get(), ta);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindComputeProgram(temporal_);
        cmd.bindTexture(3, ctx.targets.get("scene.depth"), pt);
        cmd.bindTexture(4, ctx.targets.get("scene.velocity"), pt);
        cmd.bindTexture(5, raw_, pt);
        cmd.bindTexture(6, hist_, ctx.engine.linearClamp());
        cmd.bindImage(0, out_, 0, rhi::Access::Write);
        cmd.dispatch(groups(out_->width(), 8), groups(out_->height(), 8));
        cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
        cmd.copyTexture(out_, hist_);
        historyValid_ = true;
    }

    Setting *technique_ = nullptr, *density_ = nullptr, *ssgi_ = nullptr, *ssgiIntensity_ = nullptr;
    rhi::Program *ssgiProg_ = nullptr, *temporal_ = nullptr, *shProg_ = nullptr;
    rhi::PipelinePtr capture_;
    rhi::BufferPtr params_, fwdParams_, probes_;
    rhi::TexturePtr cube_, cubeDepth_;
    rhi::Texture *out_ = nullptr, *raw_ = nullptr, *hist_ = nullptr;
    glm::ivec3 dim_{8, 4, 8};
    glm::vec3 gridMin_{0.0f}, gridSize_{1.0f};
    glm::vec4 lastSun_{0.0f};
    uint64_t geomVersion_ = 0;
    int nextProbe_ = 0;
    float scale_ = 1.0f;
    uint32_t lastW_ = 0;
    bool baked_ = false, ssgiActive_ = false, historyValid_ = false;
};

GFX_REGISTER_MODULE(GIModule);

}  // namespace gfx::lighting
