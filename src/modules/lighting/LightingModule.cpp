// LightingModule ("lighting.pbr") - the hub of the lighting area:
//   * PBR / light-quality / clear coat / emissive / ambient settings (docs/pbr_shading.md,
//     docs/light_quality.md, docs/emissive_lighting.md)
//   * uploads LightingUBO (UBO 6) once per frame, binds shade.glsl resources for everyone
//   * tiled deferred resolve -> scene.color (Stage::Lighting, order 100)
//   * HiZ pyramid for SSR/SSGI (order 15) and the lit-color history they reflect (order 190)
#include <algorithm>
#include <cmath>

#include "core/engine.h"
#include "modules/lighting/LightingShared.h"

namespace gfx::lighting {

class LightingModule : public Module {
public:
    const char* name() const override { return "lighting.pbr"; }
    int priority() const override { return 10; }   // after the other lighting modules' update()

    void registerSettings(SettingsRegistry& s) override {
        quality_ = &s.addEnum({.id = "lighting.lights.quality", .label = "Light Quality", .category = "Lighting",
                               .group = "Lights",
                               .tooltip = "How many dynamic lights are shaded per pixel and how accurately (BRDF, tiled culling, area highlights).",
                               .doc = "light_quality"},
                              {"Low", "Medium", "High", "Ultra"}, 2)
                       .presets(0, 1, 2, 3);
        maxLights_ = &s.addInt({.id = "lighting.lights.max", .label = "Max Dynamic Lights", .category = "Lighting",
                                .group = "Lights", .tooltip = "Lights beyond this count (by importance) are skipped.",
                                .doc = "light_quality"}, 64, 0, 256)
                         .presets(4, 16, 64, 256);
        clearCoat_ = &s.addBool({.id = "lighting.pbr.clearcoat", .label = "Clear Coat Layer", .category = "Lighting",
                                 .group = "Materials",
                                 .tooltip = "Second glossy lobe for car paint / lacquer materials.",
                                 .doc = "pbr_shading"}, true)
                         .presets(0, 0, 1, 1);
        emissive_ = &s.addFloat({.id = "lighting.emissive.intensity", .label = "Emissive Intensity", .category = "Lighting",
                                 .group = "Materials", .tooltip = "Scales self-lit materials (screens, neon, lava).",
                                 .doc = "emissive_lighting", .flags = SettingNoPreset}, 1.0f, 0.0f, 10.0f);
        ambient_ = &s.addFloat({.id = "lighting.ambient.intensity", .label = "Sky / IBL Intensity", .category = "Lighting",
                                .group = "Indirect", .tooltip = "Strength of image-based (environment) lighting.",
                                .doc = "pbr_shading", .flags = SettingNoPreset}, 1.0f, 0.0f, 4.0f);
    }

    void init(Engine& e) override {
        auto& dev = e.device();
        auto& sh = e.shaders();
        resolve_ = sh.compute("lighting/deferred_lighting.comp");
        hizProg_ = sh.compute("lighting/hiz_build.comp");
        copyProg_ = sh.compute("lighting/copy_color.comp");
        lutProg_ = sh.compute("lighting/brdf_lut.comp");

        ubo_ = dev.createBuffer({.size = sizeof(LightingUBO), .usage = rhi::BufUsageUniform, .name = "lighting.ubo", .owner = name()});
        uboNeutral_ = dev.createBuffer({.size = sizeof(LightingUBO), .usage = rhi::BufUsageUniform, .name = "lighting.uboNeutral", .owner = name()});
        passUBO_ = dev.createBuffer({.size = 64, .usage = rhi::BufUsageUniform, .name = "lighting.pass", .owner = name()});
        lut_ = dev.createBuffer({.size = sizeof(float) * 2 * kBrdfLutSize * kBrdfLutSize, .usage = rhi::BufUsageStorage,
                                 .dynamic = false, .structStride = 8, .name = "lighting.brdfLut", .owner = name()});
        shared().brdfLut = lut_.get();

        const uint8_t white[4] = {255, 255, 255, 255};
        white_ = dev.createTexture({.format = rhi::Format::RGBA8, .name = "lighting.white", .owner = name()}, white);
        const uint16_t zero[4] = {0, 0, 0, 0};   // half floats
        black_ = dev.createTexture({.format = rhi::Format::RGBA16F, .name = "lighting.black", .owner = name()}, zero);
        dummy_ = dev.createTexture({.format = rhi::Format::RGBA16F, .usage = rhi::TexUsageSampled | rhi::TexUsageStorage,
                                    .name = "lighting.dummyStorage", .owner = name()}, zero);
        shared().white = white_.get();
        shared().blackZeroAlpha = black_.get();
        shared().dummyStorage = dummy_.get();
        // fallback shadow array so sampler2DArrayShadow always has a depth texture bound
        shadowFallback_ = dev.createTexture({.type = rhi::TextureType::Tex2DArray, .format = rhi::Format::D32F,
                                             .usage = rhi::TexUsageSampled | rhi::TexUsageDepthStencil,
                                             .name = "lighting.shadowFallback", .owner = name()});
        rhi::SamplerDesc cmp;
        cmp.addressU = cmp.addressV = cmp.addressW = rhi::AddressMode::Clamp;
        cmp.useMips = false;
        cmp.compare = true;
        cmp.compareOp = rhi::CompareOp::LessEqual;
        fallbackCmp_ = dev.createSampler(cmp);
        rhi::SamplerDesc lin;
        lin.addressU = lin.addressV = lin.addressW = rhi::AddressMode::Clamp;
        trilinear_ = dev.createSampler(lin);
        fallbackCube_ = dev.createTexture({.type = rhi::TextureType::TexCube, .format = rhi::Format::RGBA16F,
                                           .depthOrLayers = 6, .name = "lighting.cubeFallback", .owner = name()});
        uint16_t greyHalf[4] = {0x3400, 0x3400, 0x3400, 0x3C00};   // 0.25 grey
        for (uint32_t f = 0; f < 6; ++f) dev.uploadTexture(fallbackCube_.get(), 0, f, greyHalf, sizeof(greyHalf));
        fallbackShadowSampler_ = fallbackCmp_.get();
    }

    void resize(Engine& e, Extent render, Extent) override {
        auto& dev = e.device();
        rhi::TextureDesc h = storageDesc(rhi::Format::R32F, "lighting.hiz", name());
        h.width = std::max(1u, render.width);
        h.height = std::max(1u, render.height);
        h.mipLevels = 0;   // full chain
        hiz_ = dev.createTexture(h);
        rhi::TextureDesc c = storageDesc(rhi::Format::RGBA16F, "lighting.prevColor", name());
        c.width = h.width;
        c.height = h.height;
        c.mipLevels = 6;
        prev_ = dev.createTexture(c);
        shared().hiz = hiz_.get();
        shared().prevColor = prev_.get();
        shared().prevColorValid = false;
    }

    void update(FrameContext& ctx) override {
        auto& u = shared().ubo;
        int q = quality_->asInt();
        u.lightCfg = glm::ivec4(q == 0 ? 0 : 1, maxLights_->asInt(), q >= 2 ? 1 : 0, q >= 3 ? 1 : 0);
        u.lightCfg2 = glm::ivec4((int)ctx.scene.lights.size(), q >= 2 ? 1 : 0,
                                 ctx.targets.get("lighting.sunVisibility") ? 1 : 0, clearCoat_->asBool() ? 1 : 0);
        float amb = ambient_->asFloat();
        u.ambient = glm::vec4(glm::vec3(0.30f, 0.38f, 0.50f) * amb, amb);
        u.ground = glm::vec4(glm::vec3(0.18f, 0.16f, 0.14f) * amb, emissive_->asFloat());
        // when our AO is off, RTAO (if any) still applies at full strength
        if (u.aoCfg.x == 0) u.aoParams.y = 1.0f;
        shared().msaaForward = ctx.msaaForward;
    }

    void addPasses(PassList& p) override {
        // Upload after core finalizes FrameConstants (order 1000) and the other modules' update().
        p.add(Stage::FrameSetup, "Lighting UBO", [this](FrameContext& ctx) {
            ctx.cmd.updateBuffer(ubo_.get(), shared().ubo);
            LightingUBO n = shared().ubo;
            n.lightCfg2.z = 0;          // no screen-space sun visibility for off-screen cameras
            n.shadowCfg.z = 0;
            n.aoParams.y = 1.0f;
            ctx.cmd.updateBuffer(uboNeutral_.get(), n);
            if (!lutBuilt_) buildLut(ctx);
        }, 1001.0f);

        p.add(Stage::Lighting, "HiZ pyramid", [this](FrameContext& ctx) { buildHiZ(ctx); }, 15.0f,
              [this] { return (shared().ssrNeedsHiZ || shared().ssgiNeedsHiZ) && !shared().msaaForward; });

        p.add(Stage::Lighting, "Deferred resolve", [this](FrameContext& ctx) { runResolve(ctx); }, 100.0f,
              [] { return !shared().msaaForward; });

        p.add(Stage::Lighting, "Lighting history", [this](FrameContext& ctx) {
            if (ctx.replaceLighting) return;
            auto* color = ctx.targets.get("scene.color");
            if (!color || !prev_) return;
            ctx.cmd.bindComputeProgram(copyProg_);
            ctx.cmd.bindTexture(0, color, ctx.engine.pointClamp());
            ctx.cmd.bindImage(0, prev_.get(), 0, rhi::Access::Write);
            ctx.cmd.dispatch(groups(prev_->width(), 8), groups(prev_->height(), 8));
            ctx.cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
            ctx.cmd.generateMips(prev_.get());
            shared().prevColorValid = true;
        }, 190.0f, [] { return shared().ssrNeedsHiZ || shared().ssgiNeedsHiZ; });
    }

    void bindShadingResources(FrameContext& ctx) override { bindShading(ctx, false); }

    // ---- called through lighting::bindShading ----
    void bind(FrameContext& ctx, bool neutral) {
        auto& S = shared();
        auto& cmd = ctx.cmd;
        rhi::Sampler* lin = ctx.engine.linearClamp();
        if (S.shadowArray) {
            cmd.bindTexture(8, S.shadowArray, S.shadowCmp);
            cmd.bindTexture(9, S.shadowArray, S.shadowRaw);
        } else {
            cmd.bindTexture(8, shadowFallback_.get(), fallbackShadowSampler_);
            cmd.bindTexture(9, shadowFallback_.get(), ctx.engine.pointClamp());
        }
        auto pick = [&](const char* n, rhi::Texture* fb) {
            rhi::Texture* t = neutral ? nullptr : ctx.targets.get(n);
            return t ? t : fb;
        };
        cmd.bindTexture(10, pick("lighting.sunVisibility", white_.get()), lin);
        cmd.bindTexture(11, pick("lighting.ao", white_.get()), lin);
        cmd.bindTexture(12, pick("lighting.contactShadows", white_.get()), lin);
        cmd.bindTexture(13, pick("lighting.indirectSpecular", black_.get()), lin);
        cmd.bindTexture(14, pick("lighting.indirectDiffuse", black_.get()), lin);
        cmd.bindTexture(15, S.envCube ? S.envCube : fallbackCube_.get(), trilinear_.get());
        cmd.bindUniformBuffer(6, neutral ? uboNeutral_.get() : ubo_.get());
        if (S.probeSSBO) cmd.bindStorageBuffer(4, S.probeSSBO);
        else cmd.bindStorageBuffer(4, lut_.get());   // never read when the grid isn't baked
        cmd.bindStorageBuffer(5, lut_.get());
    }

private:
    void buildLut(FrameContext& ctx) {
        ctx.cmd.bindComputeProgram(lutProg_);
        ctx.cmd.bindStorageBuffer(5, lut_.get());
        ctx.cmd.dispatch(groups(kBrdfLutSize, 8), groups(kBrdfLutSize, 8));
        ctx.cmd.barrier(rhi::BarrierStorage);
        lutBuilt_ = true;
    }

    void buildHiZ(FrameContext& ctx) {
        if (ctx.replaceLighting || !hiz_) return;
        auto* depth = ctx.targets.get("scene.depth");
        uint32_t mips = hiz_->desc().mipLevels;
        if (mips == 0) mips = 1 + (uint32_t)std::floor(std::log2((float)std::max(hiz_->width(), hiz_->height())));
        ctx.cmd.bindComputeProgram(hizProg_);
        for (uint32_t m = 0; m < mips; ++m) {
            glm::ivec4 pp(m == 0 ? -1 : (int)m - 1, 0, 0, 0);
            ctx.cmd.updateBuffer(passUBO_.get(), pp);
            ctx.cmd.bindUniformBuffer(3, passUBO_.get());
            ctx.cmd.bindTexture(6, m == 0 ? depth : hiz_.get(), ctx.engine.pointClamp());
            ctx.cmd.bindImage(0, hiz_.get(), m, rhi::Access::Write);
            ctx.cmd.dispatch(groups(hiz_->width(m), 8), groups(hiz_->height(m), 8));
            ctx.cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
        }
    }

    void runResolve(FrameContext& ctx) {
        if (ctx.replaceLighting) return;
        auto& cmd = ctx.cmd;
        auto& S = shared();
        rhi::Sampler* pt = ctx.engine.pointClamp();
        cmd.bindComputeProgram(resolve_);
        bindShading(ctx, false);
        cmd.bindTexture(0, ctx.targets.get("gbuffer.albedo"), pt);
        cmd.bindTexture(1, ctx.targets.get("gbuffer.normal"), pt);
        cmd.bindTexture(2, ctx.targets.get("gbuffer.material"), pt);
        cmd.bindTexture(3, ctx.targets.get("gbuffer.emissive"), pt);
        cmd.bindTexture(4, ctx.targets.get("scene.depth"), pt);
        cmd.bindTexture(5, S.planar ? S.planar : black_.get(), ctx.engine.linearClamp());
        cmd.bindImage(0, ctx.targets.get("scene.color"), 0, rhi::Access::Write);
        cmd.bindImage(1, S.sssDiffuse ? S.sssDiffuse : dummy_.get(), 0, rhi::Access::Write);
        cmd.dispatch(groups(ctx.renderSize.width, 16), groups(ctx.renderSize.height, 16));
        cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
    }

    Setting *quality_ = nullptr, *maxLights_ = nullptr, *clearCoat_ = nullptr, *emissive_ = nullptr, *ambient_ = nullptr;
    rhi::Program *resolve_ = nullptr, *hizProg_ = nullptr, *copyProg_ = nullptr, *lutProg_ = nullptr;
    rhi::BufferPtr ubo_, uboNeutral_, passUBO_, lut_;
    rhi::TexturePtr white_, black_, dummy_, shadowFallback_, fallbackCube_, hiz_, prev_;
    rhi::SamplerPtr fallbackCmp_, trilinear_;
    rhi::Sampler* fallbackShadowSampler_ = nullptr;
    bool lutBuilt_ = false;

    friend void lighting::bindShading(FrameContext&, bool);
};

static LightingModule* g_hub = nullptr;

void bindShading(FrameContext& ctx, bool neutralScreen) {
    if (!g_hub) g_hub = static_cast<LightingModule*>(ctx.engine.findModule("lighting.pbr"));
    if (g_hub) g_hub->bind(ctx, neutralScreen);
}

GFX_REGISTER_MODULE(LightingModule);

}  // namespace gfx::lighting
