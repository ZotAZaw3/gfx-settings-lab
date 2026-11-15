// Ray tracing module: RT shadows, RT AO, RT reflections, RT GI, path tracing, SVGF denoiser.
//
// Everything runs as compute shaders against a CPU-built BVH (bvh.cpp) uploaded as SSBOs 10-13,
// so the same code runs on the OpenGL 4.5 and D3D11 backends. Hardware RT (DXR) is documented in
// docs/raytracing_overview.md as "how it would map".
//
// Frame flow:
//   FrameSetup (order 50): rebuild/upload BVH when geometry or instance transforms change,
//                          create/remove shared lighting inputs, set ctx.replaceLighting for PT.
//   Lighting   (order 50): per enabled effect: trace -> temporal -> a-trous -> composite into
//                          lighting.sunVisibility / lighting.ao / lighting.indirectDiffuse /
//                          lighting.indirectSpecular (read by the lighting thread's resolve at 100).
//              (order 60): path tracer -> denoise -> scene.color.
//              (order 900): export rt.noisy / rt.albedo for Ray Reconstruction.
//              (order 990): debug view.
#include "core/engine.h"
#include "modules/raytracing/bvh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace gfx {
namespace {

using rhi::Access;
using rhi::Format;

constexpr uint32_t kSlotNodes = 10, kSlotTris = 11, kSlotAttrs = 12, kSlotMaterials = 13;
constexpr uint32_t kSlotParams = 10;

// Mirrors `RTParams` in shaders/raytracing/rt_common.glsl (std140).
struct RTParams {
    glm::mat4 invViewProj;
    glm::mat4 invViewProjCam;
    glm::mat4 prevViewProj;
    glm::vec4 camPos;
    glm::vec4 sunDir;
    glm::vec4 sunRadiance;
    glm::vec4 skyZenith;
    glm::vec4 skyHorizon;
    glm::ivec4 frameInfo;
    glm::vec4 effectParams;
    glm::ivec4 sizes;
    glm::vec4 denoiseParams;
    glm::ivec4 misc;
};
static_assert(sizeof(RTParams) % 16 == 0, "std140");

enum FlagBits : int {
    kFlagRussianRoulette = 1,
    kFlagAccumulate = 2,
    kFlagHistoryValid = 4,
    kFlagFireflyClamp = 8,
};

enum class Effect : int { Shadows = 0, AO, Reflections, GI, PathTracing, Count };
constexpr int kEffectCount = int(Effect::Count);

const char* kEffectKey[kEffectCount] = {"shadows", "ao", "reflections", "gi", "pt"};

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Average color of a scene texture = its smallest CPU mip (1x1 when the chain is complete).
glm::vec4 averageTexel(const Scene& scene, int texIndex) {
    if (texIndex < 0 || texIndex >= int(scene.textures.size())) return glm::vec4(1.0f);
    const SceneTexture& t = scene.textures[size_t(texIndex)];
    if (t.cpuMips.empty() || t.cpuMips.back().size() < 4) return glm::vec4(1.0f);
    const std::vector<uint8_t>& m = t.cpuMips.back();
    glm::dvec4 sum(0.0);
    size_t px = m.size() / 4;
    for (size_t i = 0; i < px; ++i)
        for (int c = 0; c < 4; ++c) sum[c] += m[i * 4 + size_t(c)] / 255.0;
    glm::vec4 avg = glm::vec4(sum / double(px));
    if (t.srgb) { avg.r = srgbToLinear(avg.r); avg.g = srgbToLinear(avg.g); avg.b = srgbToLinear(avg.b); }
    return avg;
}

}  // namespace

class RayTracingModule : public Module {
public:
    const char* name() const override { return "raytracing"; }
    int priority() const override { return 20; }

    // ------------------------------------------------------------------ settings
    void registerSettings(SettingsRegistry& s) override {
        const std::string cat = "Ray Tracing";
        settings_ = &s;

        quality_ = &s.addEnum({.id = "rt.quality", .label = "RT Quality Preset", .category = cat, .group = "General",
                               .tooltip = "Sets rays per pixel, bounces, RT resolution and roughness cutoff together.",
                               .doc = "rt_ray_budget", .flags = SettingNoPreset},
                              {"Custom", "Low", "Medium", "High", "Ultra", "Psycho"}, 0)
                       .changed([this](Setting& q) { applyQuality(q.asInt()); });
        scale_ = &s.addFloat({.id = "rt.resolution", .label = "RT Resolution Scale", .category = cat, .group = "General",
                              .tooltip = "Trace at a fraction of render resolution, then upsample. Cost scales with pixel count.",
                              .doc = "rt_ray_budget", .flags = SettingRequiresResize}, 0.5f, 0.25f, 1.0f)
                     .presets(0.5f, 0.5f, 0.75f, 1.0f).fmt("%.2f");
        maxDist_ = &s.addFloat({.id = "rt.max_distance", .label = "Max Ray Distance (m)", .category = cat, .group = "General",
                                .tooltip = "Rays stop after this distance (shadows, reflections, GI).", .doc = "rt_ray_budget"},
                               200.0f, 10.0f, 1000.0f).fmt("%.0f");
        dynamicBvh_ = &s.addBool({.id = "rt.bvh.dynamic", .label = "Rebuild BVH When Objects Move", .category = cat, .group = "General",
                                  .tooltip = "Off = BVH only rebuilt when geometry changes; moving objects keep stale RT shadows/reflections.",
                                  .doc = "raytracing_overview"}, true);
        leafSize_ = &s.addInt({.id = "rt.bvh.leaf_size", .label = "BVH Max Leaf Triangles", .category = cat, .group = "General",
                               .tooltip = "Triangles per BVH leaf. Smaller = deeper tree, more box tests, fewer triangle tests.",
                               .doc = "raytracing_overview", .flags = SettingDebug}, 4, 1, 16)
                        .changed([this](Setting&) { bvhDirty_ = true; });

        // Shadows
        shadows_ = &s.addBool({.id = "rt.shadows", .label = "RT Shadows", .category = cat, .group = "RT Shadows",
                               .tooltip = "Sun shadows from occlusion rays: no resolution limit, physically soft penumbras.",
                               .doc = "raytracing_shadows"}, false).presets(0, 0, 0, 1);
        shadowRays_ = &s.addInt({.id = "rt.shadows.rays", .label = "Shadow Rays per Pixel", .category = cat, .group = "RT Shadows",
                                 .doc = "raytracing_shadows"}, 1, 1, 4).visible([this] { return shadows_->asBool(); });
        sunSize_ = &s.addFloat({.id = "rt.shadows.sun_radius", .label = "Sun Angular Radius (deg)", .category = cat, .group = "RT Shadows",
                                .tooltip = "Bigger sun = softer shadows. Real sun is 0.27 deg. 0 = hard shadows.",
                                .doc = "raytracing_shadows"}, 0.5f, 0.0f, 3.0f)
                       .visible([this] { return shadows_->asBool() || pt_->asBool(); });

        // AO
        ao_ = &s.addBool({.id = "rt.ao", .label = "RT Ambient Occlusion", .category = cat, .group = "RT Ambient Occlusion",
                          .tooltip = "Short occlusion rays over the hemisphere. No screen-space halos. Disabled while RT GI is on.",
                          .doc = "raytracing_ao"}, false)
                  .presets(0, 0, 1, 1).enabled([this] { return !gi_->asBool(); });
        aoRays_ = &s.addInt({.id = "rt.ao.rays", .label = "AO Rays per Pixel", .category = cat, .group = "RT Ambient Occlusion",
                             .doc = "raytracing_ao"}, 1, 1, 8).visible([this] { return ao_->asBool(); });
        aoRadius_ = &s.addFloat({.id = "rt.ao.radius", .label = "AO Radius (m)", .category = cat, .group = "RT Ambient Occlusion",
                                 .doc = "raytracing_ao"}, 1.0f, 0.1f, 5.0f).visible([this] { return ao_->asBool(); });

        // Reflections
        refl_ = &s.addBool({.id = "rt.reflections", .label = "RT Reflections", .category = cat, .group = "RT Reflections",
                            .tooltip = "Reflections traced into the full scene: no screen-space holes.",
                            .doc = "raytracing_reflections"}, false).presets(0, 0, 1, 1);
        reflRays_ = &s.addInt({.id = "rt.reflections.rays", .label = "Reflection Rays per Pixel", .category = cat, .group = "RT Reflections",
                               .doc = "raytracing_reflections"}, 1, 1, 2).visible([this] { return refl_->asBool(); });
        reflRough_ = &s.addFloat({.id = "rt.reflections.max_roughness", .label = "Max Roughness", .category = cat, .group = "RT Reflections",
                                  .tooltip = "Rougher surfaces fall back to SSR / environment probe (biggest saving).",
                                  .doc = "raytracing_reflections"}, 0.6f, 0.05f, 1.0f)
                         .presets(0.3f, 0.5f, 0.7f, 1.0f).visible([this] { return refl_->asBool(); });
        reflBounces_ = &s.addInt({.id = "rt.reflections.bounces", .label = "Reflection Bounces", .category = cat, .group = "RT Reflections",
                                  .tooltip = "2 = reflections inside reflections (mirror facing mirror).",
                                  .doc = "raytracing_reflections"}, 1, 1, 2).visible([this] { return refl_->asBool(); });

        // GI
        gi_ = &s.addBool({.id = "rt.gi", .label = "RT Global Illumination", .category = cat, .group = "RT Global Illumination",
                          .tooltip = "Diffuse bounce light from rays. Includes occlusion, so RT AO is turned off.",
                          .doc = "raytracing_gi"}, false).presets(0, 0, 0, 1);
        giRays_ = &s.addInt({.id = "rt.gi.rays", .label = "GI Rays per Pixel", .category = cat, .group = "RT Global Illumination",
                             .doc = "raytracing_gi"}, 1, 1, 2).visible([this] { return gi_->asBool(); });
        giBounces_ = &s.addInt({.id = "rt.gi.bounces", .label = "GI Bounces", .category = cat, .group = "RT Global Illumination",
                                .tooltip = "1 = single bounce. 2+ = adds last frame's GI at hit points (infinite-bounce approximation).",
                                .doc = "raytracing_gi"}, 2, 1, 3).visible([this] { return gi_->asBool(); });

        // Path tracing
        pt_ = &s.addBool({.id = "rt.pathtracing", .label = "Path Tracing", .category = cat, .group = "Path Tracing",
                          .tooltip = "Replaces all raster lighting with a Monte Carlo path tracer (full RT).",
                          .doc = "path_tracing", .flags = SettingNoPreset}, false)
                  .changed([this](Setting&) { resetPt_ = true; });
        ptSpp_ = &s.addInt({.id = "rt.pathtracing.spp", .label = "Samples per Pixel per Frame", .category = cat, .group = "Path Tracing",
                            .doc = "path_tracing"}, 1, 1, 8).visible([this] { return pt_->asBool(); })
                     .changed([this](Setting&) { resetPt_ = true; });
        ptBounces_ = &s.addInt({.id = "rt.pathtracing.bounces", .label = "Max Bounces", .category = cat, .group = "Path Tracing",
                                .doc = "path_tracing"}, 4, 1, 8).visible([this] { return pt_->asBool(); })
                         .changed([this](Setting&) { resetPt_ = true; });
        ptRR_ = &s.addBool({.id = "rt.pathtracing.russian_roulette", .label = "Russian Roulette", .category = cat, .group = "Path Tracing",
                            .tooltip = "Randomly ends weak paths after 2 bounces (unbiased, much cheaper deep bounces).",
                            .doc = "path_tracing"}, true).visible([this] { return pt_->asBool(); })
                    .changed([this](Setting&) { resetPt_ = true; });
        ptAccum_ = &s.addBool({.id = "rt.pathtracing.accumulate", .label = "Accumulate When Still", .category = cat, .group = "Path Tracing",
                               .tooltip = "Average frames while the camera is still: converges to a noise-free reference.",
                               .doc = "path_tracing"}, true).visible([this] { return pt_->asBool(); })
                       .changed([this](Setting&) { resetPt_ = true; });
        ptClamp_ = &s.addBool({.id = "rt.pathtracing.firefly_clamp", .label = "Firefly Clamp", .category = cat, .group = "Path Tracing",
                               .tooltip = "Clamps very bright samples (biased, removes isolated white pixels).",
                               .doc = "path_tracing"}, true).visible([this] { return pt_->asBool(); })
                       .changed([this](Setting&) { resetPt_ = true; });

        // Denoiser
        denoiser_ = &s.addEnum({.id = "rt.denoiser", .label = "RT Denoiser", .category = cat, .group = "Denoising",
                                .tooltip = "Cleans 1-ray-per-pixel noise using neighbours and previous frames. Skipped when Ray Reconstruction is on.",
                                .doc = "denoising"}, {"Off", "Temporal Only", "SVGF"}, 2);
        dnAlpha_ = &s.addFloat({.id = "rt.denoiser.temporal_alpha", .label = "Temporal Alpha (min)", .category = cat, .group = "Denoising",
                                .tooltip = "Lower = longer history, smoother but more ghosting.",
                                .doc = "denoising"}, 0.05f, 0.01f, 0.5f).visible([this] { return denoiser_->asInt() > 0; });
        dnIters_ = &s.addInt({.id = "rt.denoiser.atrous_iterations", .label = "A-Trous Iterations", .category = cat, .group = "Denoising",
                              .tooltip = "Spatial filter passes (step 1, 2, 4, 8, 16 px).",
                              .doc = "denoising"}, 4, 1, 5).visible([this] { return denoiser_->asInt() == 2; });
        dnSigmaL_ = &s.addFloat({.id = "rt.denoiser.sigma_luminance", .label = "Luminance Edge Sigma", .category = cat, .group = "Denoising",
                                 .doc = "denoising", .flags = SettingDebug}, 4.0f, 0.5f, 16.0f);

        // Documented-only techniques (greyed out, "?" opens the doc)
        auto docOnly = [&](const char* id, const char* label, const char* group, const char* doc, const char* tip) {
            s.addBool({.id = id, .label = label, .category = cat, .group = group, .tooltip = tip, .doc = doc,
                       .flags = SettingNoPreset}, false).enabled([] { return false; });
        };
        docOnly("rt.restir_di", "ReSTIR DI (many lights)", "Advanced (documented)", "restir",
                "Reservoir resampling for thousands of lights. Needs a many-light scene; see doc.");
        docOnly("rt.restir_gi", "ReSTIR GI", "Advanced (documented)", "restir", "Reuses GI samples across pixels and frames.");
        docOnly("rt.caustics", "RT Caustics (photon)", "Advanced (documented)", "rt_caustics_translucency",
                "Light-traced caustics through glass/water. Refraction itself is in the path tracer.");
        docOnly("rt.ser", "Shader Execution Reordering / Ray Sorting", "Advanced (documented)", "shader_execution_reordering",
                "RTX 40+ hardware feature (DXR 1.2); software wavefront sorting not in the megakernel.");
        docOnly("rt.omm", "Opacity Micromaps", "Advanced (documented)", "opacity_micromaps",
                "Hardware alpha-test acceleration for foliage. Compute BVH treats alpha-masked geometry as opaque.");

        // Touching any knob the quality preset controls flips "RT Quality Preset" back to Custom.
        for (Setting* k : {scale_, shadowRays_, aoRays_, reflRays_, reflRough_, giRays_, giBounces_, ptSpp_, ptBounces_})
            k->changed([this](Setting&) { if (!applyingQuality_ && quality_->asInt() != 0) quality_->setNumber(0.0f); });

        debug_ = &s.addEnum({.id = "rt.debug_view", .label = "RT Debug View", .category = cat, .group = "Debug",
                             .tooltip = "Show a raw or denoised RT buffer full screen.", .doc = "denoising",
                             .flags = SettingNoPreset | SettingDebug},
                            {"Off", "Shadows", "AO", "Reflections", "GI", "Shadows (raw)", "AO (raw)", "Reflections (raw)", "GI (raw)"}, 0);
    }

    void applyQuality(int q) {
        if (q <= 0) return;
        // Low, Medium, High, Ultra, Psycho (docs/rt_ray_budget.md table)
        struct Row { float scale; int sh, ao, refl; float rough; int gi, giB, spp, ptB; };
        static const Row rows[5] = {
            {0.50f, 1, 1, 1, 0.3f, 1, 1, 1, 2},
            {0.50f, 1, 1, 1, 0.5f, 1, 1, 1, 3},
            {0.75f, 1, 2, 1, 0.7f, 1, 2, 1, 4},
            {1.00f, 2, 2, 1, 1.0f, 2, 2, 2, 5},
            {1.00f, 4, 4, 2, 1.0f, 2, 3, 4, 8},
        };
        const Row& r = rows[q - 1];
        applyingQuality_ = true;
        scale_->setNumber(r.scale);
        shadowRays_->setNumber(float(r.sh));
        aoRays_->setNumber(float(r.ao));
        reflRays_->setNumber(float(r.refl));
        reflRough_->setNumber(r.rough);
        giRays_->setNumber(float(r.gi));
        giBounces_->setNumber(float(r.giB));
        ptSpp_->setNumber(float(r.spp));
        ptBounces_->setNumber(float(r.ptB));
        applyingQuality_ = false;
    }

    // ------------------------------------------------------------------ init / resize
    void init(Engine& e) override {
        auto& sh = e.shaders();
        progShadows_ = sh.compute("raytracing/rt_shadows.comp");
        progAO_ = sh.compute("raytracing/rt_ao.comp");
        progRefl_ = sh.compute("raytracing/rt_reflections.comp");
        progGI_ = sh.compute("raytracing/rt_gi.comp");
        progPT_ = sh.compute("raytracing/rt_pathtrace.comp");
        progTemporal_ = sh.compute("raytracing/rt_denoise_temporal.comp");
        progAtrous_ = sh.compute("raytracing/rt_denoise_atrous.comp");
        progExport_ = sh.compute("raytracing/rt_export_noisy.comp");
        compScalar_ = e.createFullscreenPipeline("raytracing/rt_composite.frag", {"COMPOSITE_SCALAR"});
        compGI_ = e.createFullscreenPipeline("raytracing/rt_composite.frag", {"COMPOSITE_GI"});
        compRefl_ = e.createFullscreenPipeline("raytracing/rt_composite.frag", {"COMPOSITE_REFL"});
        compPT_ = e.createFullscreenPipeline("raytracing/rt_composite.frag", {"COMPOSITE_PT"});
        compDebug_ = e.createFullscreenPipeline("raytracing/rt_composite.frag", {"COMPOSITE_DEBUG"});
        params_ = e.device().createBuffer({.size = sizeof(RTParams), .usage = rhi::BufUsageUniform,
                                           .name = "rt.params", .owner = name()});
    }

    void resize(Engine&, Extent render, Extent) override {
        render_ = render;
        // Size-dependent textures are created lazily per effect (see ensureEffectTargets) so a
        // disabled effect costs no VRAM; here we only drop them so they are recreated at the new size.
        for (auto& et : effects_) et.created = false;
        historyValid_.fill(false);
        resetPt_ = true;
        needsTargetRecreate_ = true;
    }

    size_t cpuMemoryBytes() const override {
        return bvh_.nodes().size() * sizeof(rt::BVHNode) + bvh_.gpuTris().size() * sizeof(rt::GpuTri) +
               bvh_.gpuAttrs().size() * sizeof(rt::GpuTriAttr) + materials_.size() * sizeof(rt::GpuMaterial);
    }

    // ------------------------------------------------------------------ passes
    void addPasses(PassList& p) override {
        p.add(Stage::FrameSetup, "RT setup / BVH", [this](FrameContext& ctx) { setup(ctx); }, 50.0f);

        auto hybridOn = [this] { return anyHybrid() && !pt_->asBool(); };
        p.add(Stage::Lighting, "RT shadows", [this](FrameContext& ctx) { runEffect(ctx, Effect::Shadows); }, 50.0f,
              [this, hybridOn] { return hybridOn() && shadows_->asBool(); });
        p.add(Stage::Lighting, "RT AO", [this](FrameContext& ctx) { runEffect(ctx, Effect::AO); }, 51.0f,
              [this, hybridOn] { return hybridOn() && aoActive(); });
        p.add(Stage::Lighting, "RT reflections", [this](FrameContext& ctx) { runEffect(ctx, Effect::Reflections); }, 52.0f,
              [this, hybridOn] { return hybridOn() && refl_->asBool(); });
        p.add(Stage::Lighting, "RT GI", [this](FrameContext& ctx) { runEffect(ctx, Effect::GI); }, 53.0f,
              [this, hybridOn] { return hybridOn() && gi_->asBool(); });
        p.add(Stage::Lighting, "Path tracing", [this](FrameContext& ctx) { runEffect(ctx, Effect::PathTracing); }, 60.0f,
              [this] { return pt_->asBool(); });
        p.add(Stage::Lighting, "RT export (ray reconstruction)", [this](FrameContext& ctx) { exportNoisy(ctx); }, 900.0f,
              [this] { return (anyHybrid() || pt_->asBool()) && rrActive(); });
        p.add(Stage::Lighting, "RT debug view", [this](FrameContext& ctx) { debugView(ctx); }, 990.0f,
              [this] { return debug_->asInt() != 0; });
    }

private:
    struct EffectTargets {
        bool created = false;
        rhi::Texture* raw = nullptr;
        rhi::Texture* accum[2] = {nullptr, nullptr};
        rhi::Texture* moments[2] = {nullptr, nullptr};
        rhi::Texture* filt[2] = {nullptr, nullptr};
        rhi::Texture* extra = nullptr;    // GI: previous final; PT: accumulation (RGBA32F)
        rhi::Texture* extra2 = nullptr;   // PT: primary albedo
        rhi::Texture* final = nullptr;    // last result (for composite / debug)
        rhi::Texture* rawFinal = nullptr; // raw signal (debug)
    };

    bool aoActive() const { return ao_->asBool() && !gi_->asBool(); }
    bool anyHybrid() const { return shadows_->asBool() || aoActive() || refl_->asBool() || gi_->asBool(); }
    bool rrActive() const {
        const Setting* rr = settings_ ? settings_->find("aa.ray_reconstruction") : nullptr;
        return rr && rr->asInt() != 0;
    }
    bool effectOn(Effect e) const {
        if (pt_->asBool()) return e == Effect::PathTracing;
        switch (e) {
            case Effect::Shadows: return shadows_->asBool();
            case Effect::AO: return aoActive();
            case Effect::Reflections: return refl_->asBool();
            case Effect::GI: return gi_->asBool();
            default: return false;
        }
    }

    Extent traceSize() const {
        float s = scale_->asFloat();
        return {std::max(1u, uint32_t(std::lround(render_.width * s))), std::max(1u, uint32_t(std::lround(render_.height * s)))};
    }

    void ensureEffectTargets(FrameContext& ctx, Effect e) {
        EffectTargets& t = effects_[int(e)];
        if (t.created) return;
        const std::string base = std::string("rt.") + kEffectKey[int(e)] + ".";
        const float s = scale_->asFloat();
        auto mk = [&](const char* n, Format f) {
            rhi::TextureDesc d;
            d.format = f;
            d.usage = rhi::TexUsageSampled | rhi::TexUsageStorage | rhi::TexUsageRenderTarget;
            d.name = base + n;
            d.owner = name();
            return ctx.targets.create(base + n, d, TargetScale::Render, s);
        };
        const bool scalar = e == Effect::Shadows || e == Effect::AO;
        // Remove stale (different scale) versions first so create() rebuilds them at the new size.
        for (const char* n : {"raw", "accum0", "accum1", "mom0", "mom1", "filt0", "filt1", "extra", "extra2"})
            ctx.targets.remove(base + n);
        t.raw = mk("raw", scalar ? Format::R16F : Format::RGBA16F);
        t.accum[0] = mk("accum0", Format::RGBA16F);
        t.accum[1] = mk("accum1", Format::RGBA16F);
        t.moments[0] = mk("mom0", Format::RGBA16F);
        t.moments[1] = mk("mom1", Format::RGBA16F);
        t.filt[0] = mk("filt0", Format::RGBA16F);
        t.filt[1] = mk("filt1", Format::RGBA16F);
        t.extra = nullptr; t.extra2 = nullptr;
        if (e == Effect::GI) t.extra = mk("extra", Format::RGBA16F);
        if (e == Effect::PathTracing) { t.extra = mk("extra", Format::RGBA32F); t.extra2 = mk("extra2", Format::RGBA16F); }
        t.created = true;
        historyValid_[int(e)] = false;
        if (e == Effect::GI) giHistory_ = false;
        if (e == Effect::PathTracing) resetPt_ = true;
    }

    void releaseEffectTargets(FrameContext& ctx, Effect e) {
        EffectTargets& t = effects_[int(e)];
        if (!t.created) return;
        const std::string base = std::string("rt.") + kEffectKey[int(e)] + ".";
        for (const char* n : {"raw", "accum0", "accum1", "mom0", "mom1", "filt0", "filt1", "extra", "extra2"})
            ctx.targets.remove(base + n);
        t = EffectTargets{};
        historyValid_[int(e)] = false;
        if (e == Effect::GI) giHistory_ = false;
    }

    // Shared lighting inputs: create while the RT effect is on; remove only if we created it.
    struct SharedInput { const char* name; Format fmt; bool ownedByUs = false; };
    std::array<SharedInput, 4> shared_ = {{{"lighting.sunVisibility", Format::R16F},
                                           {"lighting.ao", Format::R8},
                                           {"lighting.indirectSpecular", Format::RGBA16F},
                                           {"lighting.indirectDiffuse", Format::RGBA16F}}};

    rhi::Texture* sharedTarget(FrameContext& ctx, int idx, bool want) {
        SharedInput& si = shared_[size_t(idx)];
        rhi::Texture* t = ctx.targets.get(si.name);
        if (want) {
            if (!t) {
                rhi::TextureDesc d;
                d.format = si.fmt;
                d.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget | rhi::TexUsageStorage;
                d.name = si.name;
                d.owner = name();
                t = ctx.targets.create(si.name, d, TargetScale::Render, 1.0f);
                si.ownedByUs = true;
            }
            return t;
        }
        if (t && si.ownedByUs) ctx.targets.remove(si.name);
        si.ownedByUs = false;
        return nullptr;
    }

    // ------------------------------------------------------------------ FrameSetup
    void setup(FrameContext& ctx) {
        render_ = ctx.renderSize;
        ctx.replaceLighting = ctx.replaceLighting || pt_->asBool();

        const bool active = anyHybrid() || pt_->asBool();
        if (active) updateBVH(ctx);

        // Effect textures: allocate on enable, free on disable (VRAM shows up per setting in the profiler).
        for (int i = 0; i < kEffectCount; ++i) {
            Effect e = Effect(i);
            if (effectOn(e)) {
                if (needsTargetRecreate_) effects_[i].created = false;
                ensureEffectTargets(ctx, e);
            } else {
                releaseEffectTargets(ctx, e);
            }
        }
        needsTargetRecreate_ = false;

        const bool hybrid = !pt_->asBool();
        sharedTarget(ctx, 0, hybrid && shadows_->asBool());
        sharedTarget(ctx, 1, hybrid && aoActive());
        sharedTarget(ctx, 2, hybrid && refl_->asBool());
        sharedTarget(ctx, 3, hybrid && gi_->asBool());

        // Path tracer accumulation resets on camera movement / scene change.
        const glm::mat4& vp = ctx.frame.unjitteredViewProj;
        if (std::memcmp(&vp, &lastViewProj_, sizeof(glm::mat4)) != 0) resetPt_ = true;
        lastViewProj_ = vp;
        if (std::fabs(ctx.frame.sunDirection.x - lastSun_.x) + std::fabs(ctx.frame.sunDirection.y - lastSun_.y) +
            std::fabs(ctx.frame.sunDirection.z - lastSun_.z) + std::fabs(ctx.frame.sunDirection.w - lastSun_.w) > 1e-5f)
            resetPt_ = true;
        lastSun_ = ctx.frame.sunDirection;

        if (rrActive() && active) {
            rhi::TextureDesc d;
            d.format = Format::RGBA16F;
            d.usage = rhi::TexUsageSampled | rhi::TexUsageStorage | rhi::TexUsageRenderTarget;
            d.owner = name();
            d.name = "rt.noisy";  noisy_ = ctx.targets.create("rt.noisy", d, TargetScale::Render, 1.0f);
            d.name = "rt.albedo"; albedoOut_ = ctx.targets.create("rt.albedo", d, TargetScale::Render, 1.0f);
        } else if (noisy_) {
            ctx.targets.remove("rt.noisy"); ctx.targets.remove("rt.albedo");
            noisy_ = albedoOut_ = nullptr;
        }
        ctx.profiler.counter("rt.bvh.triangles", double(bvh_.stats().triangles));
        ctx.profiler.counter("rt.bvh.nodes", double(bvh_.stats().nodes));
        ctx.profiler.counter("rt.bvh.build_ms", bvh_.stats().buildMs);
        if (pt_->asBool()) ctx.profiler.counter("rt.pt.accumulated_frames", double(ptFrames_));
    }

    void updateBVH(FrameContext& ctx) {
        Scene& sc = ctx.scene;
        bool rebuild = bvhDirty_ || sc.geometryVersion() != geomVersion_ || !nodesBuf_;
        if (!rebuild && dynamicBvh_->asBool()) {
            if (sc.instances.size() != lastWorlds_.size()) rebuild = true;
            else
                for (size_t i = 0; i < sc.instances.size() && !rebuild; ++i)
                    if (std::memcmp(&sc.instances[i].world, &lastWorlds_[i], sizeof(glm::mat4)) != 0) rebuild = true;
        }
        if (sc.materialVersion() != matVersion_ || !matBuf_) uploadMaterials(ctx);
        if (!rebuild) return;

        GFX_CPU_SCOPE(ctx.profiler, "BVH build");
        std::vector<rt::BuildTriangle> tris;
        for (const Instance& inst : sc.instances) {
            if (inst.mesh >= sc.meshes.size()) continue;
            glm::mat3 nrm = glm::transpose(glm::inverse(glm::mat3(inst.world)));
            for (uint32_t pi : sc.meshes[inst.mesh].primitives) {
                if (pi >= sc.primitives.size()) continue;
                const Primitive& prim = sc.primitives[pi];
                if (prim.lods.empty()) continue;
                const LodLevel& lod = prim.lods[0];
                for (uint32_t k = 0; k + 2 < lod.indexCount; k += 3) {
                    rt::BuildTriangle bt;
                    for (int v = 0; v < 3; ++v) {
                        const Vertex& vx = sc.vertices[prim.baseVertex + sc.indices[lod.firstIndex + k + uint32_t(v)]];
                        bt.p[v] = glm::vec3(inst.world * glm::vec4(vx.position, 1.0f));
                        bt.n[v] = glm::normalize(nrm * vx.normal);
                    }
                    bt.material = uint32_t(std::max(prim.material, 0));
                    tris.push_back(bt);
                }
            }
        }
        bvh_.build(tris, uint32_t(leafSize_->asInt()));

        auto& dev = ctx.device;
        auto mkBuf = [&](const char* n, const void* data, size_t bytes, uint32_t stride) {
            static const uint8_t zero[64] = {};
            if (bytes == 0) { data = zero; bytes = stride; }
            return dev.createBuffer({.size = bytes, .usage = rhi::BufUsageStorage, .dynamic = false,
                                     .structStride = stride, .name = n, .owner = name()}, data);
        };
        nodesBuf_ = mkBuf("rt.bvh.nodes", bvh_.nodes().data(), bvh_.nodes().size() * sizeof(rt::BVHNode), sizeof(rt::BVHNode));
        trisBuf_ = mkBuf("rt.bvh.tris", bvh_.gpuTris().data(), bvh_.gpuTris().size() * sizeof(rt::GpuTri), sizeof(rt::GpuTri));
        attrsBuf_ = mkBuf("rt.bvh.attrs", bvh_.gpuAttrs().data(), bvh_.gpuAttrs().size() * sizeof(rt::GpuTriAttr), sizeof(rt::GpuTriAttr));

        geomVersion_ = sc.geometryVersion();
        lastWorlds_.resize(sc.instances.size());
        for (size_t i = 0; i < sc.instances.size(); ++i) lastWorlds_[i] = sc.instances[i].world;
        bvhDirty_ = false;
        resetPt_ = true;
    }

    void uploadMaterials(FrameContext& ctx) {
        const Scene& sc = ctx.scene;
        materials_.clear();
        for (const Material& m : sc.materials) {
            glm::vec4 base = m.baseColor * averageTexel(sc, m.baseColorTex);
            glm::vec4 mr = averageTexel(sc, m.metalRoughTex);   // glTF: g = roughness, b = metallic
            glm::vec3 emissive = m.emissive;
            if (m.emissiveTex >= 0) emissive *= glm::vec3(averageTexel(sc, m.emissiveTex));
            rt::GpuMaterial g;
            g.albedoRoughness = glm::vec4(glm::vec3(base), glm::clamp(m.roughness * mr.g, 0.02f, 1.0f));
            g.emissiveMetallic = glm::vec4(emissive, glm::clamp(m.metallic * mr.b, 0.0f, 1.0f));
            // Alpha-blended materials become glass in the path tracer (transmission = 1 - alpha).
            float transmission = m.alphaMode == AlphaMode::Blend ? glm::clamp(1.0f - m.baseColor.a, 0.0f, 1.0f) : 0.0f;
            g.transmissionIor = glm::vec4(transmission, 1.5f, 0.0f, 0.0f);
            materials_.push_back(g);
        }
        if (materials_.empty()) materials_.push_back({glm::vec4(0.8f, 0.8f, 0.8f, 0.5f), glm::vec4(0.0f), glm::vec4(0.0f, 1.5f, 0, 0)});
        matBuf_ = ctx.device.createBuffer({.size = materials_.size() * sizeof(rt::GpuMaterial), .usage = rhi::BufUsageStorage,
                                           .dynamic = false, .structStride = sizeof(rt::GpuMaterial),
                                           .name = "rt.materials", .owner = name()}, materials_.data());
        matVersion_ = sc.materialVersion();
        resetPt_ = true;
    }

    // ------------------------------------------------------------------ helpers
    RTParams baseParams(FrameContext& ctx, Extent trace) const {
        const FrameConstants& f = ctx.frame;
        RTParams p{};
        p.invViewProj = f.invViewProj;
        p.invViewProjCam = glm::inverse(f.unjitteredViewProj);
        p.prevViewProj = f.prevViewProj;
        p.camPos = glm::vec4(glm::vec3(f.cameraPos), float(ptFrames_));
        float sunRadius = glm::radians(sunSize_->asFloat());
        p.sunDir = glm::vec4(glm::normalize(glm::vec3(f.sunDirection)), sunRadius);
        p.sunRadiance = glm::vec4(glm::vec3(f.sunColor) * f.sunDirection.w, 0.0f);
        // Simple analytic sky scaled with the sun (sun intensity 6 -> sky ~1).
        float k = f.sunDirection.w * 0.16f * glm::clamp(f.sunDirection.y * 2.0f + 0.2f, 0.05f, 1.0f);
        p.skyZenith = glm::vec4(glm::vec3(0.30f, 0.50f, 1.00f) * k, 0.0f);
        p.skyHorizon = glm::vec4(glm::vec3(0.75f, 0.82f, 0.95f) * k, 0.0f);
        p.frameInfo = glm::ivec4(int(ctx.frameIndex & 0x7fffffff), 1, 1, 0);
        p.effectParams = glm::vec4(aoRadius_->asFloat(), reflRough_->asFloat(), maxDist_->asFloat(), 0.01f);
        p.sizes = glm::ivec4(int(trace.width), int(trace.height), int(ctx.renderSize.width), int(ctx.renderSize.height));
        p.denoiseParams = glm::vec4(dnAlpha_->asFloat(), 1.0f, 0.02f, dnSigmaL_->asFloat());
        p.misc = glm::ivec4(0, rrActive() ? 1 : 0, 0, 0);
        return p;
    }

    void bindScene(FrameContext& ctx) {
        auto& c = ctx.cmd;
        c.bindStorageBuffer(kSlotNodes, nodesBuf_.get());
        c.bindStorageBuffer(kSlotTris, trisBuf_.get());
        c.bindStorageBuffer(kSlotAttrs, attrsBuf_.get());
        c.bindStorageBuffer(kSlotMaterials, matBuf_.get());
    }
    void bindGBuffer(FrameContext& ctx) {
        auto& c = ctx.cmd;
        rhi::Sampler* pt = ctx.engine.pointClamp();
        c.bindTexture(0, ctx.targets.get("scene.depth"), pt);
        c.bindTexture(1, ctx.targets.get("gbuffer.normal"), pt);
        c.bindTexture(2, ctx.targets.get("gbuffer.material"), pt);
        c.bindTexture(3, ctx.targets.get("gbuffer.albedo"), pt);
    }
    void setParams(FrameContext& ctx, const RTParams& p) {
        ctx.cmd.updateBuffer(params_.get(), p);
        ctx.cmd.bindUniformBuffer(kSlotParams, params_.get());
    }
    static void dispatch2D(FrameContext& ctx, Extent e) {
        ctx.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8, 1);
    }

    // Temporal + optional a-trous. Returns the texture holding the final result.
    rhi::Texture* denoise(FrameContext& ctx, Effect e, RTParams p, Extent trace, rhi::Texture* input, bool temporal) {
        EffectTargets& t = effects_[int(e)];
        auto& c = ctx.cmd;
        const int mode = denoiser_->asInt();
        if (mode == 0 || rrActive()) { historyValid_[int(e)] = false; return input; }

        rhi::Texture* src = input;
        if (temporal) {
            const int cur = int(ctx.frameIndex & 1), prev = cur ^ 1;
            if (historyValid_[int(e)]) p.frameInfo.w |= kFlagHistoryValid;
            setParams(ctx, p);
            c.bindComputeProgram(progTemporal_);
            bindGBuffer(ctx);
            c.bindTexture(4, input, ctx.engine.pointClamp());
            c.bindTexture(5, t.accum[prev], ctx.engine.linearClamp());
            c.bindTexture(6, t.moments[prev], ctx.engine.pointClamp());
            c.bindImage(0, t.accum[cur], 0, Access::Write);
            c.bindImage(1, t.moments[cur], 0, Access::Write);
            dispatch2D(ctx, trace);
            c.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
            historyValid_[int(e)] = true;
            src = t.accum[cur];
            if (mode == 1) return src;

            // SVGF feeds the first a-trous iteration back as history; we keep it simple and filter
            // a copy so the history stays the temporally accumulated signal.
            const int iters = dnIters_->asInt();
            for (int i = 0; i < iters; ++i) {
                rhi::Texture* dst = t.filt[i & 1];
                p.denoiseParams.y = float(1 << i);
                setParams(ctx, p);
                c.bindComputeProgram(progAtrous_);
                bindGBuffer(ctx);
                c.bindTexture(4, src, ctx.engine.pointClamp());
                c.bindImage(0, dst, 0, Access::Write);
                dispatch2D(ctx, trace);
                c.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
                if (i == 0) { c.copyTexture(dst, t.accum[cur]); }   // SVGF: filtered history
                src = dst;
            }
            return src;
        }
        // Spatial only (path tracer: its own accumulation replaces the temporal pass).
        if (mode < 2) return src;
        const int iters = dnIters_->asInt();
        for (int i = 0; i < iters; ++i) {
            rhi::Texture* dst = t.filt[i & 1];
            p.denoiseParams.y = float(1 << i);
            setParams(ctx, p);
            c.bindComputeProgram(progAtrous_);
            bindGBuffer(ctx);
            c.bindTexture(4, src, ctx.engine.pointClamp());
            c.bindImage(0, dst, 0, Access::Write);
            dispatch2D(ctx, trace);
            c.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
            src = dst;
        }
        return src;
    }

    void composite(FrameContext& ctx, rhi::Pipeline* pipe, rhi::Texture* src, rhi::Texture* target, const RTParams& p,
                   const char* label, rhi::Texture* albedo = nullptr) {
        if (!target || !src) return;
        setParams(ctx, p);
        ctx.cmd.bindTexture(2, ctx.targets.get("gbuffer.material"), ctx.engine.linearClamp());
        ctx.cmd.bindTexture(4, src, ctx.engine.linearClamp());
        ctx.cmd.bindTexture(5, albedo ? albedo : src, ctx.engine.linearClamp());
        ctx.fullscreen(pipe, target, label);
    }

    // ------------------------------------------------------------------ effects
    void runEffect(FrameContext& ctx, Effect e) {
        if (!nodesBuf_ || !matBuf_) return;
        ensureEffectTargets(ctx, e);
        EffectTargets& t = effects_[int(e)];
        Extent trace = traceSize();
        RTParams p = baseParams(ctx, trace);
        auto& c = ctx.cmd;
        c.pushDebugGroup(kEffectKey[int(e)]);

        rhi::Program* prog = nullptr;
        switch (e) {
            case Effect::Shadows: prog = progShadows_; p.frameInfo.y = shadowRays_->asInt(); break;
            case Effect::AO: prog = progAO_; p.frameInfo.y = aoRays_->asInt(); break;
            case Effect::Reflections:
                prog = progRefl_; p.frameInfo.y = reflRays_->asInt(); p.frameInfo.z = reflBounces_->asInt(); break;
            case Effect::GI:
                prog = progGI_; p.frameInfo.y = giRays_->asInt(); p.frameInfo.z = giBounces_->asInt();
                if (giHistory_) p.frameInfo.w |= kFlagHistoryValid;
                break;
            case Effect::PathTracing:
                prog = progPT_; p.frameInfo.y = ptSpp_->asInt(); p.frameInfo.z = ptBounces_->asInt();
                if (ptRR_->asBool()) p.frameInfo.w |= kFlagRussianRoulette;
                if (ptAccum_->asBool()) p.frameInfo.w |= kFlagAccumulate;
                if (ptClamp_->asBool()) p.frameInfo.w |= kFlagFireflyClamp;
                if (resetPt_) { ptFrames_ = 0; resetPt_ = false; }
                else p.frameInfo.w |= kFlagHistoryValid;
                p.camPos.w = float(ptFrames_);
                break;
            default: break;
        }

        setParams(ctx, p);
        c.bindComputeProgram(prog);
        bindScene(ctx);
        bindGBuffer(ctx);
        if (e == Effect::PathTracing) {
            c.bindImage(0, t.extra, 0, Access::ReadWrite);
            c.bindImage(1, t.raw, 0, Access::Write);
            c.bindImage(2, t.extra2, 0, Access::Write);
        } else {
            if (e == Effect::GI) c.bindTexture(4, t.extra, ctx.engine.linearClamp());
            c.bindImage(0, t.raw, 0, Access::Write);
        }
        dispatch2D(ctx, trace);
        c.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
        t.rawFinal = t.raw;

        p.frameInfo.w &= ~kFlagHistoryValid;
        if (e == Effect::PathTracing) {
            ++ptFrames_;
            // Denoise only while the accumulation is young; a converged image needs no filter.
            rhi::Texture* res = (ptFrames_ < 64) ? denoise(ctx, e, p, trace, t.raw, false) : t.raw;
            t.final = res;
            composite(ctx, compPT_.get(), res, ctx.targets.get("scene.color"), p, "PT -> scene.color", t.extra2);
        } else {
            rhi::Texture* res = denoise(ctx, e, p, trace, t.raw, true);
            t.final = res;
            switch (e) {
                case Effect::Shadows: composite(ctx, compScalar_.get(), res, ctx.targets.get("lighting.sunVisibility"), p, "RT shadows composite"); break;
                case Effect::AO: composite(ctx, compScalar_.get(), res, ctx.targets.get("lighting.ao"), p, "RTAO composite"); break;
                case Effect::Reflections: composite(ctx, compRefl_.get(), res, ctx.targets.get("lighting.indirectSpecular"), p, "RT reflections composite"); break;
                case Effect::GI:
                    composite(ctx, compGI_.get(), res, ctx.targets.get("lighting.indirectDiffuse"), p, "RTGI composite");
                    // Keep this frame's GI for next frame's multi-bounce lookups.
                    if (res->desc().format == t.extra->desc().format) c.copyTexture(res, t.extra);
                    else c.blitTexture(res, t.extra, rhi::Filter::Nearest);
                    giHistory_ = true;
                    break;
                default: break;
            }
        }
        c.popDebugGroup();
    }

    void exportNoisy(FrameContext& ctx) {
        if (!noisy_ || !albedoOut_) return;
        auto& c = ctx.cmd;
        c.bindComputeProgram(progExport_);
        c.bindTexture(0, ctx.targets.get("scene.color"), ctx.engine.pointClamp());
        rhi::Texture* alb = pt_->asBool() && effects_[int(Effect::PathTracing)].extra2
                                ? effects_[int(Effect::PathTracing)].extra2 : ctx.targets.get("gbuffer.albedo");
        c.bindTexture(1, alb, ctx.engine.linearClamp());
        c.bindImage(0, noisy_, 0, Access::Write);
        c.bindImage(1, albedoOut_, 0, Access::Write);
        dispatch2D(ctx, ctx.renderSize);
        c.barrier(rhi::BarrierTextureFetch | rhi::BarrierImage);
    }

    void debugView(FrameContext& ctx) {
        int v = debug_->asInt();
        bool raw = v >= 5;
        int idx = raw ? v - 5 : v - 1;   // 0 shadows, 1 AO, 2 reflections, 3 GI
        EffectTargets& t = effects_[idx];
        rhi::Texture* src = raw ? t.rawFinal : t.final;
        if (!t.created || !src) return;
        RTParams p = baseParams(ctx, traceSize());
        p.misc.x = (idx <= 1) ? 1 : 0;
        composite(ctx, compDebug_.get(), src, ctx.targets.get("scene.color"), p, "RT debug view");
    }

    // ------------------------------------------------------------------ state
    SettingsRegistry* settings_ = nullptr;
    Setting *quality_ = nullptr, *scale_ = nullptr, *maxDist_ = nullptr, *dynamicBvh_ = nullptr, *leafSize_ = nullptr;
    Setting *shadows_ = nullptr, *shadowRays_ = nullptr, *sunSize_ = nullptr;
    Setting *ao_ = nullptr, *aoRays_ = nullptr, *aoRadius_ = nullptr;
    Setting *refl_ = nullptr, *reflRays_ = nullptr, *reflRough_ = nullptr, *reflBounces_ = nullptr;
    Setting *gi_ = nullptr, *giRays_ = nullptr, *giBounces_ = nullptr;
    Setting *pt_ = nullptr, *ptSpp_ = nullptr, *ptBounces_ = nullptr, *ptRR_ = nullptr, *ptAccum_ = nullptr, *ptClamp_ = nullptr;
    Setting *denoiser_ = nullptr, *dnAlpha_ = nullptr, *dnIters_ = nullptr, *dnSigmaL_ = nullptr;
    Setting* debug_ = nullptr;
    bool applyingQuality_ = false;

    rhi::Program *progShadows_ = nullptr, *progAO_ = nullptr, *progRefl_ = nullptr, *progGI_ = nullptr, *progPT_ = nullptr;
    rhi::Program *progTemporal_ = nullptr, *progAtrous_ = nullptr, *progExport_ = nullptr;
    rhi::PipelinePtr compScalar_, compGI_, compRefl_, compPT_, compDebug_;
    rhi::BufferPtr params_, nodesBuf_, trisBuf_, attrsBuf_, matBuf_;

    rt::BVH bvh_;
    std::vector<rt::GpuMaterial> materials_;
    std::vector<glm::mat4> lastWorlds_;
    uint64_t geomVersion_ = 0, matVersion_ = 0;
    bool bvhDirty_ = true;

    std::array<EffectTargets, kEffectCount> effects_{};
    std::array<bool, kEffectCount> historyValid_{};
    bool giHistory_ = false;
    bool needsTargetRecreate_ = false;
    rhi::Texture *noisy_ = nullptr, *albedoOut_ = nullptr;

    Extent render_{};
    glm::mat4 lastViewProj_{0.0f};
    glm::vec4 lastSun_{0.0f};
    bool resetPt_ = true;
    uint32_t ptFrames_ = 0;
};

GFX_REGISTER_MODULE(RayTracingModule);

}  // namespace gfx
