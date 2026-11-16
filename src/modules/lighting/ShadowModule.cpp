// ShadowModule ("lighting.shadows") - cascaded sun shadow maps with HW PCF / PCF 3x3 / Poisson /
// PCSS filtering, screen-space contact shadows and capsule (character) shadows.
// Docs: docs/shadows.md, docs/shadow_resolution_distance.md, docs/capsule_shadows.md
#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "core/engine.h"
#include "modules/lighting/LightingShared.h"

namespace gfx::lighting {

class ShadowModule : public Module {
public:
    const char* name() const override { return "lighting.shadows"; }

    void registerSettings(SettingsRegistry& s) override {
        const char* cat = "Shadows";
        quality_ = &s.addEnum({.id = "lighting.shadows.quality", .label = "Shadow Quality", .category = cat, .group = "Sun Shadows",
                               .tooltip = "Master switch for sun shadows; picks cascades, resolution and filter together.",
                               .doc = "shadows"}, {"Off", "Low", "Medium", "High", "Ultra"}, 3)
                       .presets(1, 2, 3, 4);
        resolution_ = &s.addEnum({.id = "lighting.shadows.resolution", .label = "Shadow Map Resolution", .category = cat,
                                  .group = "Sun Shadows", .tooltip = "Texels per side of each cascade. Sharper edges, more VRAM.",
                                  .doc = "shadow_resolution_distance"}, {"512", "1024", "2048", "4096"}, 2)
                          .presets(1, 1, 2, 3)
                          .visible([this] { return quality_->asInt() != 0; });
        cascades_ = &s.addInt({.id = "lighting.shadows.cascades", .label = "Shadow Cascades", .category = cat, .group = "Sun Shadows",
                               .tooltip = "Slices of the view distance, each with its own shadow map.",
                               .doc = "shadows"}, 4, 1, 4)
                        .presets(1, 3, 4, 4)
                        .visible([this] { return quality_->asInt() != 0; });
        distance_ = &s.addFloat({.id = "lighting.shadows.distance", .label = "Shadow Distance", .category = cat, .group = "Sun Shadows",
                                 .tooltip = "How far from the camera shadows are drawn (meters).",
                                 .doc = "shadow_resolution_distance"}, 150.0f, 10.0f, 500.0f)
                        .presets(40, 80, 150, 300)
                        .fmt("%.0f m")
                        .visible([this] { return quality_->asInt() != 0; });
        filter_ = &s.addEnum({.id = "lighting.shadows.filter", .label = "Shadow Filtering", .category = cat, .group = "Sun Shadows",
                              .tooltip = "Edge softness: hardware 2x2, 3x3 PCF, 16-tap Poisson, or PCSS (contact-hardening).",
                              .doc = "shadows"}, {"Hardware PCF", "PCF 3x3", "Poisson PCF", "PCSS"}, 2)
                      .presets(0, 1, 2, 3)
                      .visible([this] { return quality_->asInt() != 0; });
        lightSize_ = &s.addFloat({.id = "lighting.shadows.lightSize", .label = "Sun Softness (PCSS)", .category = cat,
                                  .group = "Sun Shadows", .tooltip = "Light source size used by PCSS penumbra estimation.",
                                  .doc = "shadows", .flags = SettingNoPreset}, 0.5f, 0.05f, 2.0f)
                         .visible([this] { return filter_->asInt() == 3; });
        update_ = &s.addEnum({.id = "lighting.shadows.farUpdate", .label = "Far Cascade Update Rate", .category = cat,
                              .group = "Sun Shadows", .tooltip = "Re-render cascades 3-4 every N frames to save GPU time.",
                              .doc = "shadow_resolution_distance"}, {"Every frame", "Every 2nd frame", "Every 4th frame"}, 1)
                      .presets(0, 1, 1, 0)
                      .visible([this] { return quality_->asInt() != 0; });
        contact_ = &s.addBool({.id = "lighting.shadows.contact", .label = "Contact Shadows", .category = cat, .group = "Detail",
                               .tooltip = "Short screen-space ray march toward the sun for small, sharp contact detail.",
                               .doc = "shadows"}, true)
                       .presets(0, 0, 1, 1);
        capsule_ = &s.addBool({.id = "lighting.shadows.capsule", .label = "Capsule Shadows", .category = cat, .group = "Detail",
                               .tooltip = "Soft analytic shadows from character-sized objects approximated as capsules.",
                               .doc = "capsule_shadows"}, true)
                       .presets(0, 1, 1, 1);
    }

    void init(Engine& e) override {
        auto& dev = e.device();
        rhi::Program* prog = e.shaders().graphics("lighting/shadow_depth.vert", "lighting/shadow_depth.frag");
        rhi::Program* progMasked = e.shaders().graphics("lighting/shadow_depth.vert", "lighting/shadow_depth.frag", {"ALPHA_TEST"});
        rhi::PipelineDesc pd;
        pd.program = prog;
        pd.layout = SceneRenderer::vertexLayout();
        pd.raster.cull = rhi::CullMode::None;          // thin geometry (cards, planes) must cast both ways
        pd.raster.depthBiasConstant = 1.5f;
        pd.raster.depthBiasSlope = 2.5f;
        pd.raster.depthClamp = true;                   // pancake casters in front of the near plane
        pd.depth.compare = rhi::CompareOp::LessEqual;  // shadow maps use standard Z
        pd.blend[0].writeMask = 0;
        pd.name = "lighting.shadowDepth";
        pipeline_ = dev.createPipeline(pd);
        pd.program = progMasked;
        pd.name = "lighting.shadowDepthMasked";
        pipelineMasked_ = dev.createPipeline(pd);
        contactProg_ = e.shaders().compute("lighting/contact_shadows.comp");
        params_ = dev.createBuffer({.size = 16, .usage = rhi::BufUsageUniform, .name = "shadows.params", .owner = name()});

        rhi::SamplerDesc c;
        c.addressU = c.addressV = c.addressW = rhi::AddressMode::Clamp;
        c.useMips = false;
        c.compare = true;
        c.compareOp = rhi::CompareOp::LessEqual;
        cmp_ = dev.createSampler(c);
        rhi::SamplerDesc r = c;
        r.compare = false;
        r.minFilter = r.magFilter = rhi::Filter::Nearest;
        raw_ = dev.createSampler(r);
        shared().shadowCmp = cmp_.get();
        shared().shadowRaw = raw_.get();
    }

    void update(FrameContext& ctx) override {
        auto& S = shared();
        auto& u = S.ubo;
        bool rtShadows = ctx.targets.get("lighting.sunVisibility") != nullptr;
        int q = quality_->asInt();
        int n = q == 0 ? 0 : std::clamp(cascades_->asInt(), 1, kMaxCascades);
        uint32_t res = 512u << resolution_->asInt();
        bool active = n > 0 && !ctx.replaceLighting && !rtShadows;

        if (active && (!array_ || array_->width() != res || (int)array_->desc().depthOrLayers != n)) {
            rhi::TextureDesc d;
            d.type = rhi::TextureType::Tex2DArray;
            d.format = rhi::Format::D32F;
            d.width = d.height = res;
            d.depthOrLayers = (uint32_t)n;
            d.usage = rhi::TexUsageSampled | rhi::TexUsageDepthStencil;
            d.name = "lighting.shadowCascades";
            d.owner = name();
            array_ = ctx.device.createTexture(d);
            for (auto& f : lastRendered_) f = -1000;
        } else if (!active && array_ && n == 0) {
            array_.reset();   // free VRAM when shadows are off
        }
        S.shadowArray = (active && array_) ? array_.get() : nullptr;

        u.shadowCfg = glm::ivec4(S.shadowArray ? n : 0, filter_->asInt(), contact_->asBool() ? 1 : 0, 0);
        u.shadowParams = glm::vec4(1.5f, lightSize_->asFloat(), distance_->asFloat(), 0.15f);
        if (S.shadowArray) fitCascades(ctx, n, res);

        // contact shadows: we are the only writer of this shared input
        bool wantContact = contact_->asBool() && !ctx.replaceLighting && !ctx.msaaForward;
        if (wantContact && !contactTarget_) {
            contactTarget_ = ctx.targets.create("lighting.contactShadows", storageDesc(rhi::Format::R8, "lighting.contactShadows", name()));
        } else if (!wantContact && contactTarget_) {
            ctx.targets.remove("lighting.contactShadows");
            contactTarget_ = nullptr;
        }
        if (contactTarget_) contactTarget_ = ctx.targets.get("lighting.contactShadows");   // survives resizes

        buildCapsules(ctx, u);
    }

    void addPasses(PassList& p) override {
        p.add(Stage::Shadow, "Shadow cascades", [this](FrameContext& ctx) { renderCascades(ctx); }, 0.0f,
              [] { return shared().shadowArray != nullptr; });
        p.add(Stage::Lighting, "Contact shadows", [this](FrameContext& ctx) {
            if (ctx.replaceLighting || !contactTarget_) return;
            glm::vec4 prm(0.35f, 0.15f, 16.0f, 0.0f);   // max distance, thickness, steps
            ctx.cmd.updateBuffer(params_.get(), prm);
            ctx.cmd.bindUniformBuffer(3, params_.get());
            ctx.cmd.bindComputeProgram(contactProg_);
            ctx.cmd.bindTexture(3, ctx.targets.get("scene.depth"), ctx.engine.pointClamp());
            ctx.cmd.bindImage(0, contactTarget_, 0, rhi::Access::Write);
            ctx.cmd.dispatch(groups(contactTarget_->width(), 8), groups(contactTarget_->height(), 8));
            ctx.cmd.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
        }, 5.0f, [this] { return contactTarget_ != nullptr; });
    }

private:
    // Practical split scheme + bounding-sphere fit + texel snapping (docs/shadows.md, Rebuild 3).
    void fitCascades(FrameContext& ctx, int n, uint32_t res) {
        auto& u = shared().ubo;
        const Camera& cam = ctx.camera;
        float nearP = cam.nearPlane;
        float farP = std::min(distance_->asFloat(), cam.farPlane);
        float aspect = ctx.renderSize.height ? (float)ctx.renderSize.width / (float)ctx.renderSize.height : 1.0f;
        float tanH = std::tan(cam.fovY * 0.5f);
        float t = tanH * tanH * (1.0f + aspect * aspect);   // squared corner slope
        glm::mat4 invView = glm::inverse(cam.viewMatrix());
        glm::vec3 L = glm::normalize(ctx.scene.sun.direction);
        glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        float sceneRadius = ctx.scene.bounds.valid() ? glm::length(ctx.scene.bounds.extent()) : 100.0f;
        int interval = 1 << update_->asInt();
        const float lambda = 0.75f;

        float prevSplit = nearP;
        for (int i = 0; i < kMaxCascades; ++i) {
            if (i >= n) { u.cascadeSplits[i] = 1e9f; continue; }
            float fi = float(i + 1) / float(n);
            float split = lambda * nearP * std::pow(farP / nearP, fi) + (1.0f - lambda) * (nearP + (farP - nearP) * fi);
            u.cascadeSplits[i] = split;
            // far cascades may skip frames: keep their last matrix so the lookup matches the map
            bool render = i < 2 || interval == 1 || (int64_t)ctx.frameIndex - lastRendered_[i] >= interval;
            renderThisFrame_[i] = render;
            if (!render) { prevSplit = split; continue; }

            float zn = prevSplit, zf = split;
            float zc = std::min(0.5f * (zf + zn) * (1.0f + t), zf);   // view distance of the sphere center
            float radius = std::sqrt(std::max((zc - zn) * (zc - zn) + zn * zn * t, (zf - zc) * (zf - zc) + zf * zf * t));
            radius = std::ceil(radius * 16.0f) / 16.0f;               // quantize: stable texel size
            glm::vec3 center = glm::vec3(invView * glm::vec4(0, 0, -zc, 1));

            float texel = 2.0f * radius / float(res);
            float back = radius + sceneRadius;                        // catch casters behind the slice
            glm::mat4 view = glm::lookAt(center + L * back, center, up);
            // snap the sphere center to the texel grid in light space
            glm::vec4 c = view * glm::vec4(center, 1.0f);
            glm::vec2 snapped = glm::floor(glm::vec2(c) / texel) * texel;
            glm::vec2 off = snapped - glm::vec2(c);
            glm::mat4 proj = glm::orthoRH_ZO(-radius + off.x, radius + off.x, -radius + off.y, radius + off.y, 0.0f, back + radius);
            u.cascadeVP[i] = proj * view;
            u.cascadeTexel[i] = texel;
            prevSplit = split;
        }
    }

    void renderCascades(FrameContext& ctx) {
        if (ctx.replaceLighting || !array_) return;
        auto& u = shared().ubo;
        for (int i = 0; i < u.shadowCfg.x; ++i) {
            if (!renderThisFrame_[i]) continue;
            rhi::RenderPassDesc rp;
            rp.depth.texture = array_.get();
            rp.depth.layer = (uint32_t)i;
            rp.depth.load = rhi::LoadOp::Clear;
            rp.depth.clearDepth = 1.0f;   // standard Z
            rp.name = "Shadow cascade";
            ctx.cmd.beginRenderPass(rp);
            SceneDrawParams dp;
            dp.mode = DrawMode::DepthOnly;
            dp.viewProj = u.cascadeVP[i];
            dp.pipeline = pipeline_.get();
            dp.maskedPipeline = pipelineMasked_.get();
            dp.shadowCastersOnly = true;
            ctx.renderer.drawScene(ctx, dp);
            ctx.cmd.endRenderPass();
            lastRendered_[i] = (int64_t)ctx.frameIndex;
        }
    }

    // Character-sized instances (0.8-2.5 m tall, narrow) become vertical capsules.
    void buildCapsules(FrameContext& ctx, LightingUBO& u) {
        int count = 0;
        if (capsule_->asBool() && !ctx.replaceLighting) {
            glm::vec3 camPos = ctx.camera.position;
            struct Cand { float d; glm::vec3 a, b; float r; };
            std::vector<Cand> c;
            for (auto& inst : ctx.scene.instances) {
                if (!inst.worldBounds.valid()) continue;
                glm::vec3 e = inst.worldBounds.extent() * 2.0f;
                float w = std::max(e.x, e.z);
                if (e.y < 0.8f || e.y > 2.5f || w > 1.0f || w < 0.1f) continue;
                glm::vec3 ctr = inst.worldBounds.center();
                float r = w * 0.5f;
                c.push_back({glm::distance(ctr, camPos), ctr - glm::vec3(0, e.y * 0.5f - r, 0), ctr + glm::vec3(0, e.y * 0.5f - r, 0), r});
            }
            std::sort(c.begin(), c.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
            for (auto& k : c) {
                if (count == 8) break;
                u.capsuleA[count] = glm::vec4(k.a, k.r);
                u.capsuleB[count] = glm::vec4(k.b, 0.0f);
                ++count;
            }
        }
        u.shadowCfg.w = count;
    }

    Setting *quality_ = nullptr, *resolution_ = nullptr, *cascades_ = nullptr, *distance_ = nullptr, *filter_ = nullptr,
            *lightSize_ = nullptr, *update_ = nullptr, *contact_ = nullptr, *capsule_ = nullptr;
    rhi::PipelinePtr pipeline_, pipelineMasked_;
    rhi::Program* contactProg_ = nullptr;
    rhi::BufferPtr params_;
    rhi::SamplerPtr cmp_, raw_;
    rhi::TexturePtr array_;
    rhi::Texture* contactTarget_ = nullptr;
    int64_t lastRendered_[kMaxCascades] = {-1000, -1000, -1000, -1000};
    bool renderThisFrame_[kMaxCascades] = {true, true, true, true};
};

GFX_REGISTER_MODULE(ShadowModule);

}  // namespace gfx::lighting
