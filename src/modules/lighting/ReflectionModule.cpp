// ReflectionModule ("lighting.reflections") - the raster reflection chain:
//   environment cubemap (procedural sky or a captured local probe, GGX-prefiltered, optional
//   box-projection), screen-space reflections (linear or HiZ tracing, temporal) into the shared
//   lighting.indirectSpecular input, and planar reflections for the ground plane.
// Docs: docs/reflections.md, docs/reflection_probe_quality.md
#include <algorithm>
#include <cmath>

#include "core/engine.h"
#include "modules/lighting/LightingShared.h"

namespace gfx::lighting {

class ReflectionModule : public Module {
public:
    const char* name() const override { return "lighting.reflections"; }

    void registerSettings(SettingsRegistry& s) override {
        const char* cat = "Lighting";
        probeRes_ = &s.addEnum({.id = "lighting.refl.probeResolution", .label = "Reflection Probe Resolution", .category = cat,
                                .group = "Reflections", .tooltip = "Cubemap size: sharpness of mirror-like reflections.",
                                .doc = "reflection_probe_quality"}, {"128", "256", "512", "1024"}, 2)
                        .presets(0, 1, 2, 3);
        probeSamples_ = &s.addEnum({.id = "lighting.refl.prefilterSamples", .label = "Probe Prefilter Samples", .category = cat,
                                    .group = "Reflections", .tooltip = "GGX samples per texel when blurring probe mips.",
                                    .doc = "reflection_probe_quality"}, {"32", "64", "128", "256"}, 2)
                            .presets(0, 1, 2, 3);
        probeMode_ = &s.addEnum({.id = "lighting.refl.probes", .label = "Reflection Probes", .category = cat, .group = "Reflections",
                                 .tooltip = "Sky only, or a captured local probe with box-projected (parallax-corrected) lookups.",
                                 .doc = "reflection_probe_quality"}, {"Sky only", "Local probe", "Local probe (realtime)"}, 1)
                         .presets(0, 1, 1, 2);
        ssr_ = &s.addEnum({.id = "lighting.refl.ssr", .label = "Screen Space Reflections", .category = cat, .group = "Reflections",
                           .tooltip = "Ray-march the depth buffer to reflect what is on screen.",
                           .doc = "reflections"}, {"Off", "Low", "Medium", "High", "Ultra"}, 2)
                   .presets(0, 0, 2, 4);
        planar_ = &s.addBool({.id = "lighting.refl.planar", .label = "Planar Reflections", .category = cat, .group = "Reflections",
                              .tooltip = "Re-render the scene mirrored for exact reflections on the ground/water plane.",
                              .doc = "reflections"}, false)
                      .presets(0, 0, 0, 1);
    }

    void init(Engine& e) override {
        auto& sh = e.shaders();
        skyProg_ = sh.compute("lighting/sky_to_cube.comp");
        prefilterProg_ = sh.compute("lighting/ibl_prefilter.comp");
        ssrProg_ = sh.compute("lighting/ssr.comp");
        temporal_ = sh.compute("lighting/temporal_accum.comp");
        params_ = e.device().createBuffer({.size = 32, .usage = rhi::BufUsageUniform, .name = "refl.params", .owner = name()});
        fwdParams_ = e.device().createBuffer({.size = 32, .usage = rhi::BufUsageUniform, .name = "refl.fwdParams", .owner = name()});
        rhi::PipelineDesc pd;
        pd.program = sh.graphics("lighting/forward_lite.vert", "lighting/forward_lite.frag");
        pd.layout = SceneRenderer::vertexLayout();
        pd.raster.cull = rhi::CullMode::None;   // mirrored / cube-face cameras flip winding
        pd.name = "lighting.forwardLite";
        forward_ = e.device().createPipeline(pd);
        rhi::SamplerDesc lin;
        lin.addressU = lin.addressV = lin.addressW = rhi::AddressMode::Clamp;
        trilinear_ = e.device().createSampler(lin);
    }

    void update(FrameContext& ctx) override {
        auto& S = shared();
        auto& u = S.ubo;
        uint32_t res = 128u << probeRes_->asInt();
        int samples = 32 << probeSamples_->asInt();
        int mode = probeMode_->asInt();
        glm::vec4 sun(ctx.scene.sun.direction * ctx.scene.sun.intensity, (float)mode);
        if (!env_ || env_->width() != res || samples != samples_ || sun != lastSun_ || mode != mode_) {
            createCubes(ctx, res);
            samples_ = samples;
            lastSun_ = sun;
            mode_ = mode;
            dirty_ = true;
            captureFace_ = 0;
        }
        if (mode == 2) dirty_ = true;   // realtime: keep recapturing, one face per frame
        S.envCube = env_.get();

        const AABB& b = ctx.scene.bounds;
        bool box = mode > 0 && b.valid();
        probePos_ = box ? b.center() + glm::vec3(0, b.extent().y * -0.5f + 1.5f, 0) : glm::vec3(0, 2, 0);
        u.reflBoxMin = glm::vec4(box ? b.min : glm::vec3(-1), box ? 1.0f : 0.0f);
        u.reflBoxMax = glm::vec4(box ? b.max : glm::vec3(1), 0.0f);
        u.reflProbePos = glm::vec4(probePos_, 0.0f);

        int q = ssr_->asInt();
        bool rtRefl = settingOn(ctx, "rt.reflections");
        ssrActive_ = q > 0 && !ctx.replaceLighting && !ctx.msaaForward && !rtRefl;
        static const int steps[5] = {0, 24, 32, 48, 64};
        static const float maxRough[5] = {0, 0.35f, 0.5f, 0.6f, 0.75f};
        u.reflCfg = glm::ivec4(1, ssrActive_ ? 1 : 0, steps[q], 0);
        u.reflParams = glm::vec4(maxRough[q], 0.3f, (float)(kPrefilterMips - 1), b.valid() ? b.min.y : 0.0f);
        S.ssrNeedsHiZ = ssrActive_;
        float scale = q <= 2 ? 0.5f : 1.0f;
        if (ssrActive_ && (!out_ || scale != ssrScale_)) {
            if (out_) releaseSSR(ctx);
            ssrScale_ = scale;
            auto d = storageDesc(rhi::Format::RGBA16F, "lighting.indirectSpecular", name());
            out_ = ctx.targets.create("lighting.indirectSpecular", d, TargetScale::Render, scale);
            d.name = "lighting.ssrRaw";
            raw_ = ctx.targets.create("lighting.ssrRaw", d, TargetScale::Render, scale);
            d.name = "lighting.ssrHistory";
            hist_ = ctx.targets.create("lighting.ssrHistory", d, TargetScale::Render, scale);
            historyValid_ = false;
        } else if (!ssrActive_ && out_) {
            releaseSSR(ctx);
        }
        if (out_) {
            out_ = ctx.targets.get("lighting.indirectSpecular");
            raw_ = ctx.targets.get("lighting.ssrRaw");
            hist_ = ctx.targets.get("lighting.ssrHistory");
            if (out_ && out_->width() != lastW_) { historyValid_ = false; lastW_ = out_->width(); }
        }

        bool planar = planar_->asBool() && !ctx.replaceLighting && !ctx.msaaForward;
        if (planar) {
            uint32_t w = std::max(1u, ctx.renderSize.width / 2), h = std::max(1u, ctx.renderSize.height / 2);
            if (!planarColor_ || planarColor_->width() != w || planarColor_->height() != h) {
                rhi::TextureDesc c;
                c.format = rhi::Format::RGBA16F; c.width = w; c.height = h;
                c.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
                c.name = "lighting.planarColor"; c.owner = name();
                planarColor_ = ctx.device.createTexture(c);
                c.format = rhi::Format::D32F; c.usage = rhi::TexUsageDepthStencil; c.name = "lighting.planarDepth";
                planarDepth_ = ctx.device.createTexture(c);
            }
        } else {
            planarColor_.reset();
            planarDepth_.reset();
        }
        S.planar = planarColor_.get();
        u.reflCfg.w = S.planar ? 1 : 0;
    }

    void addPasses(PassList& p) override {
        p.add(Stage::Shadow, "Environment probe", [this](FrameContext& ctx) { updateEnv(ctx); }, 50.0f,
              [this] { return dirty_; });
        p.add(Stage::Shadow, "Planar reflection", [this](FrameContext& ctx) { renderPlanar(ctx); }, 60.0f,
              [] { return shared().planar != nullptr; });
        p.add(Stage::Lighting, "SSR", [this](FrameContext& ctx) { runSSR(ctx); }, 20.0f,
              [this] { return ssrActive_ && out_ != nullptr; });
    }

private:
    static constexpr int kPrefilterMips = 6;

    void createCubes(FrameContext& ctx, uint32_t res) {
        rhi::TextureDesc d;
        d.type = rhi::TextureType::TexCube;
        d.format = rhi::Format::RGBA16F;
        d.width = d.height = res;
        d.depthOrLayers = 6;
        d.mipLevels = 0;
        d.usage = rhi::TexUsageSampled | rhi::TexUsageStorage | rhi::TexUsageRenderTarget;
        d.name = "lighting.envSource";
        d.owner = name();
        src_ = ctx.device.createTexture(d);
        d.mipLevels = kPrefilterMips;
        d.usage = rhi::TexUsageSampled | rhi::TexUsageStorage;
        d.name = "lighting.envPrefiltered";
        env_ = ctx.device.createTexture(d);
        rhi::TextureDesc z;
        z.format = rhi::Format::D32F;
        z.width = z.height = res;
        z.usage = rhi::TexUsageDepthStencil;
        z.name = "lighting.envDepth";
        z.owner = name();
        depth_ = ctx.device.createTexture(z);
        skyDone_ = false;
    }

    void prefilter(FrameContext& ctx) {
        auto& cmd = ctx.cmd;
        cmd.generateMips(src_.get());
        cmd.bindComputeProgram(prefilterProg_);
        cmd.bindTexture(6, src_.get(), trilinear_.get());
        for (int m = 0; m < kPrefilterMips; ++m) {
            glm::vec4 pf((float)m / (kPrefilterMips - 1), (float)samples_, 0, 0);
            cmd.updateBuffer(params_.get(), pf);
            cmd.bindUniformBuffer(3, params_.get());
            cmd.bindImage(0, env_.get(), (uint32_t)m, rhi::Access::Write);
            cmd.dispatch(groups(env_->width(m), 8), groups(env_->height(m), 8), 6);
            cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
        }
    }

    void updateEnv(FrameContext& ctx) {
        auto& cmd = ctx.cmd;
        // 1) sky into the source cube (also the background for local captures)
        if (!skyDone_ || mode_ == 0) {
            glm::ivec4 sky(0);
            cmd.updateBuffer(params_.get(), sky);
            cmd.bindUniformBuffer(3, params_.get());
            cmd.bindComputeProgram(skyProg_);
            cmd.bindImage(0, src_.get(), 0, rhi::Access::Write);
            cmd.dispatch(groups(src_->width(), 8), groups(src_->height(), 8), 6);
            cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage | rhi::BarrierFramebuffer);
            skyDone_ = true;
            if (mode_ == 0) { prefilter(ctx); dirty_ = false; return; }
            prefilter(ctx);   // usable sky while the local capture fills in
        }
        // 2) local probe: render the scene into one cube face per frame (time-sliced)
        int face = captureFace_;
        rhi::RenderPassDesc rp;
        rp.colors[0].texture = src_.get();
        rp.colors[0].layer = (uint32_t)face;
        rp.colors[0].load = rhi::LoadOp::Load;   // keep the sky where nothing is drawn
        rp.colorCount = 1;
        rp.depth.texture = depth_.get();
        rp.depth.load = rhi::LoadOp::Clear;
        rp.depth.clearDepth = 0.0f;
        rp.name = "Probe face";
        // background: the sky pass above filled every face; re-run it for this face when realtime
        if (mode_ == 2) {   // only this face, so the other captured faces survive
            glm::ivec4 sky(0, face, 0, 0);
            cmd.updateBuffer(params_.get(), sky);
            cmd.bindUniformBuffer(3, params_.get());
            cmd.bindComputeProgram(skyProg_);
            cmd.bindImage(0, src_.get(), 0, rhi::Access::Write);
            cmd.dispatch(groups(src_->width(), 8), groups(src_->height(), 8), 1);
            cmd.barrier(rhi::BarrierFramebuffer | rhi::BarrierImage);
        }
        cmd.beginRenderPass(rp);
        drawForwardLite(ctx, cubeFaceViewProj(face, probePos_, 0.05f), glm::vec4(0, 0, 0, 1), probePos_);
        cmd.endRenderPass();
        captureFace_ = (captureFace_ + 1) % 6;
        if (captureFace_ == 0) {
            prefilter(ctx);
            if (mode_ != 2) dirty_ = false;
        }
    }

    void drawForwardLite(FrameContext& ctx, const glm::mat4& viewProj, glm::vec4 clipPlane, glm::vec3 eye) {
        struct { glm::vec4 plane, eye; } fp{clipPlane, glm::vec4(eye, 1)};
        ctx.cmd.updateBuffer(fwdParams_.get(), fp);
        ctx.cmd.bindUniformBuffer(3, fwdParams_.get());
        bindShading(ctx, true);
        SceneDrawParams dp;
        dp.mode = DrawMode::Custom;
        dp.viewProj = viewProj;
        dp.prevViewProj = viewProj;
        dp.pipeline = forward_.get();
        ctx.renderer.drawScene(ctx, dp);
    }

    void renderPlanar(FrameContext& ctx) {
        if (ctx.replaceLighting || !planarColor_) return;
        float h = shared().ubo.reflParams.w;
        // reflection across the plane y = h
        glm::mat4 M(1.0f);
        M[1][1] = -1.0f;
        M[3][1] = 2.0f * h;
        glm::mat4 vp = ctx.frame.viewProj * M;
        glm::vec3 eye = glm::vec3(ctx.frame.cameraPos);
        eye.y = 2.0f * h - eye.y;
        rhi::RenderPassDesc rp;
        rp.colors[0].texture = planarColor_.get();
        rp.colors[0].load = rhi::LoadOp::Clear;
        rp.colorCount = 1;
        rp.depth.texture = planarDepth_.get();
        rp.depth.load = rhi::LoadOp::Clear;
        rp.depth.clearDepth = 0.0f;
        rp.name = "Planar reflection";
        ctx.cmd.beginRenderPass(rp);
        drawForwardLite(ctx, vp, glm::vec4(0, 1, 0, -h - 0.01f), eye);
        ctx.cmd.endRenderPass();
    }

    void releaseSSR(FrameContext& ctx) {
        for (const char* n : {"lighting.indirectSpecular", "lighting.ssrRaw", "lighting.ssrHistory"}) ctx.targets.remove(n);
        out_ = raw_ = hist_ = nullptr;
    }

    void runSSR(FrameContext& ctx) {
        if (ctx.replaceLighting || !shared().prevColorValid) {
            float zero[4] = {0, 0, 0, 0};
            if (out_) ctx.cmd.clearTexture(out_, zero);
            return;
        }
        auto& cmd = ctx.cmd;
        rhi::Sampler* pt = ctx.engine.pointClamp();
        glm::vec2 size(out_->width(), out_->height());
        int q = ssr_->asInt();
        glm::vec4 sp(size, q >= 4 ? 1.0f : 0.0f, 0);
        cmd.updateBuffer(params_.get(), sp);
        cmd.bindUniformBuffer(3, params_.get());
        cmd.bindComputeProgram(ssrProg_);
        cmd.bindTexture(1, ctx.targets.get("gbuffer.normal"), pt);
        cmd.bindTexture(2, ctx.targets.get("gbuffer.material"), pt);
        cmd.bindTexture(3, ctx.targets.get("scene.depth"), pt);
        cmd.bindTexture(5, shared().prevColor, trilinear_.get());
        cmd.bindTexture(7, shared().hiz, pt);
        cmd.bindImage(0, raw_, 0, rhi::Access::Write);
        cmd.dispatch(groups(raw_->width(), 8), groups(raw_->height(), 8));
        cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);

        glm::vec4 ta(size, 0.2f, historyValid_ ? 0.0f : 1.0f);
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

    Setting *probeRes_ = nullptr, *probeSamples_ = nullptr, *probeMode_ = nullptr, *ssr_ = nullptr, *planar_ = nullptr;
    rhi::Program *skyProg_ = nullptr, *prefilterProg_ = nullptr, *ssrProg_ = nullptr, *temporal_ = nullptr;
    rhi::PipelinePtr forward_;
    rhi::BufferPtr params_, fwdParams_;
    rhi::SamplerPtr trilinear_;
    rhi::TexturePtr src_, env_, depth_, planarColor_, planarDepth_;
    rhi::Texture *out_ = nullptr, *raw_ = nullptr, *hist_ = nullptr;
    glm::vec4 lastSun_{0.0f};
    glm::vec3 probePos_{0.0f};
    int samples_ = 0, mode_ = -1, captureFace_ = 0;
    float ssrScale_ = 1.0f;
    uint32_t lastW_ = 0;
    bool dirty_ = true, skyDone_ = false, ssrActive_ = false, historyValid_ = false;
};

GFX_REGISTER_MODULE(ReflectionModule);

}  // namespace gfx::lighting
