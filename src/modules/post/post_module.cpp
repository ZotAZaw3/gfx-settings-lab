// Post-processing module: the whole camera/lens/film/display stack after lighting.
//
//   PostRender (render res)  heat haze, depth of field (bokeh), motion blur
//   PostOutput (output res)  panini projection, eye adaptation, god rays, bloom + lens dirt,
//                            lens flare / sun glare, rain on lens, output composite
//   ToneMap                  exposure + tonemapping (ACES / AgX / Reinhard / Hable)
//   PostLDR                  chromatic aberration, color grading LUT, vignette, film grain,
//                            letterbox, HDR calibration pattern
//
// Docs: docs/post_processing_overview.md and one doc per effect (see the `doc` field of each setting).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "core/engine.h"
#include "post_params.h"

namespace gfx::post {

namespace {
constexpr const char* kCat = "Post Processing";
constexpr int kMaxBloomMips = 8;
constexpr int kLutSize = 32;
}  // namespace

class PostModule : public Module {
public:
    const char* name() const override { return "post"; }

    // ------------------------------------------------------------------------------------
    // Settings
    // ------------------------------------------------------------------------------------
    void registerSettings(SettingsRegistry& r) override {
        reg_ = &r;
        auto B = [&](const char* id, const char* label, const char* group, const char* doc, const char* tip, bool def) -> Setting& {
            return add(id, r.addBool({.id = std::string("post.") + id, .label = label, .category = kCat, .group = group, .tooltip = tip, .doc = doc}, def));
        };
        auto F = [&](const char* id, const char* label, const char* group, const char* doc, const char* tip, float def, float mn, float mx) -> Setting& {
            return add(id, r.addFloat({.id = std::string("post.") + id, .label = label, .category = kCat, .group = group, .tooltip = tip, .doc = doc}, def, mn, mx));
        };
        auto I = [&](const char* id, const char* label, const char* group, const char* doc, const char* tip, int def, int mn, int mx) -> Setting& {
            return add(id, r.addInt({.id = std::string("post.") + id, .label = label, .category = kCat, .group = group, .tooltip = tip, .doc = doc}, def, mn, mx));
        };
        auto E = [&](const char* id, const char* label, const char* group, const char* doc, const char* tip, std::vector<std::string> opts, int def) -> Setting& {
            return add(id, r.addEnum({.id = std::string("post.") + id, .label = label, .category = kCat, .group = group, .tooltip = tip, .doc = doc}, std::move(opts), def));
        };
        auto on = [this](const char* id) { return [this, id] { return s(id)->asBool(); }; };

        // Master
        const char* G = "Pipeline";
        B("enabled", "Post-processing", G, "post_processing_overview", "Master switch for every camera/lens/film effect (tonemapping always runs).", true);
        E("quality", "Post-processing quality", G, "post_processing_overview", "Scales sample counts, bloom mip count and blur resolution of every post effect.",
          {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3);
        E("debug", "Post debug view", G, "post_processing_overview", "Show one effect in isolation.",
          {"Off", "Bloom only", "Lens flare only", "God rays only", "DoF circle of confusion"}, 0).desc.flags = SettingDebug | SettingNoPreset;

        // Exposure
        G = "Exposure";
        B("exposure.auto", "Auto exposure (eye adaptation)", G, "exposure_eye_adaptation", "Measures scene brightness and adapts exposure over time like an eye or camera.", true);
        F("exposure.compensation", "Exposure compensation (EV)", G, "exposure_eye_adaptation", "Brightness offset in camera stops (+1 = twice as bright).", 0.0f, -5.0f, 5.0f).desc.flags = SettingNoPreset;
        F("exposure.min", "Min EV", G, "exposure_eye_adaptation", "Darkest scenes are never brightened beyond this.", -6.0f, -16.0f, 0.0f).visible(on("exposure.auto"));
        F("exposure.max", "Max EV", G, "exposure_eye_adaptation", "Brightest scenes are never darkened beyond this.", 6.0f, 0.0f, 16.0f).visible(on("exposure.auto"));
        F("exposure.speedUp", "Adapt speed (to bright)", G, "exposure_eye_adaptation", "How fast the eye adjusts when it gets brighter.", 3.0f, 0.1f, 10.0f).visible(on("exposure.auto"));
        F("exposure.speedDown", "Adapt speed (to dark)", G, "exposure_eye_adaptation", "How fast the eye adjusts when it gets darker (slower in reality).", 1.0f, 0.1f, 10.0f).visible(on("exposure.auto"));
        F("exposure.key", "Middle grey key", G, "exposure_eye_adaptation", "Target average brightness (0.18 = photographic middle grey).", 0.18f, 0.03f, 0.5f).visible(on("exposure.auto"));
        F("exposure.center", "Center-weighted metering", G, "exposure_eye_adaptation", "0 = whole frame counts equally, 1 = mostly the centre.", 0.5f, 0.0f, 1.0f).visible(on("exposure.auto"));

        // Tonemapping
        G = "Tonemapping";
        E("tonemap", "Tonemapper", G, "tonemapping_hdr", "Curve that compresses HDR light into what the display can show.",
          {"None (clamp)", "Reinhard", "Reinhard extended", "Hable filmic", "ACES", "AgX"}, 4);
        F("tonemap.white", "White point", G, "tonemapping_hdr", "Scene value that maps to pure white (Reinhard extended / Hable).", 6.0f, 1.0f, 20.0f)
            .visible([this] { int t = s("tonemap")->asInt(); return t == 2 || t == 3; });

        // Bloom
        G = "Bloom";
        B("bloom", "Bloom", G, "bloom", "Glow that bleeds from very bright areas.", true).presets(0, 1, 1, 1);
        F("bloom.intensity", "Bloom intensity", G, "bloom", "Strength of the glow.", 0.06f, 0.0f, 0.5f).visible(on("bloom"));
        F("bloom.threshold", "Bloom threshold", G, "bloom", "Brightness where glow starts (0 = physically based, everything blooms a little).", 1.0f, 0.0f, 10.0f).visible(on("bloom"));
        F("bloom.knee", "Soft knee", G, "bloom", "Softens the threshold so glow fades in instead of cutting on.", 0.5f, 0.0f, 1.0f).visible(on("bloom"));
        F("bloom.radius", "Bloom radius", G, "bloom", "Spread of each upsample step: wider, softer glow.", 1.0f, 0.5f, 3.0f).visible(on("bloom"));
        F("bloom.dirt", "Lens dirt", G, "lens_dirt", "Smudges on the lens light up when bright light shines through them.", 0.4f, 0.0f, 2.0f)
            .presets(0.0f, 0.0f, 0.4f, 0.4f).visible(on("bloom"));

        // Lens flare & sun glare
        G = "Lens Flare & Sun Glare";
        B("flare", "Lens flare", G, "lens_flare_sun_glare", "Ghosts and halo from light bouncing between lens elements.", true).presets(0, 0, 1, 1);
        F("flare.intensity", "Flare intensity", G, "lens_flare_sun_glare", "Brightness of ghosts and halo.", 0.15f, 0.0f, 2.0f).visible(on("flare"));
        I("flare.ghosts", "Ghost count", G, "lens_flare_sun_glare", "Number of ghost images.", 5, 1, 10).visible(on("flare"));
        F("flare.spacing", "Ghost spacing", G, "lens_flare_sun_glare", "Distance between ghosts along the flare line.", 0.35f, 0.05f, 1.0f).visible(on("flare"));
        F("flare.halo", "Halo width", G, "lens_flare_sun_glare", "Radius of the halo ring.", 0.45f, 0.1f, 0.8f).visible(on("flare"));
        F("flare.ca", "Chromatic distortion", G, "lens_flare_sun_glare", "Rainbow fringing of the ghosts.", 0.008f, 0.0f, 0.03f).visible(on("flare")).fmt("%.3f");
        B("glare", "Sun glare (starburst)", G, "lens_flare_sun_glare", "Diffraction starburst and glow around the visible sun.", true).presets(0, 1, 1, 1);
        F("glare.intensity", "Glare intensity", G, "lens_flare_sun_glare", "Brightness of the sun glow and streaks.", 0.5f, 0.0f, 3.0f).visible(on("glare"));
        F("glare.streaks", "Starburst blades", G, "lens_flare_sun_glare", "Number of streaks (aperture blade count).", 6.0f, 2.0f, 16.0f).visible(on("glare")).fmt("%.0f");
        F("glare.size", "Glare size", G, "lens_flare_sun_glare", "Radius of the glow around the sun.", 0.25f, 0.05f, 1.0f).visible(on("glare"));
        F("glare.anamorphic", "Anamorphic streak", G, "lens_flare_sun_glare", "Long horizontal cinema-lens streak through the sun.", 0.0f, 0.0f, 2.0f).visible(on("glare"));

        // God rays
        G = "God Rays";
        B("rays", "God rays", G, "god_rays", "Light shafts streaming from the sun through gaps.", true).presets(0, 1, 1, 1);
        F("rays.intensity", "God ray intensity", G, "god_rays", "Brightness of the shafts.", 0.6f, 0.0f, 3.0f).visible(on("rays"));
        F("rays.density", "Ray length (density)", G, "god_rays", "How far the shafts reach from the sun.", 0.9f, 0.1f, 1.0f).visible(on("rays"));
        F("rays.decay", "Ray decay", G, "god_rays", "Falloff along each shaft.", 0.965f, 0.8f, 1.0f).visible(on("rays")).fmt("%.3f");
        F("rays.weight", "Ray weight", G, "god_rays", "Energy per sample.", 0.5f, 0.05f, 2.0f).visible(on("rays"));

        // Depth of field
        G = "Depth of Field";
        B("dof", "Depth of field", G, "depth_of_field", "Blurs what is nearer or farther than the focus distance, with bokeh highlights.", false);
        B("dof.autofocus", "Auto focus (center)", G, "depth_of_field", "Focus on whatever is in the screen centre, smoothly.", true).visible(on("dof"));
        F("dof.focus", "Focus distance (m)", G, "depth_of_field", "Distance that is perfectly sharp.", 5.0f, 0.2f, 200.0f)
            .visible([this] { return s("dof")->asBool() && !s("dof.autofocus")->asBool(); });
        F("dof.fstop", "Aperture (f-stop)", G, "depth_of_field", "Lower = wider aperture = shallower focus, bigger bokeh.", 2.8f, 1.0f, 22.0f).visible(on("dof"));
        F("dof.focal", "Focal length (mm)", G, "depth_of_field", "Longer lenses blur the background more.", 50.0f, 12.0f, 300.0f).visible(on("dof")).fmt("%.0f");
        F("dof.maxBlur", "Max blur radius (px)", G, "depth_of_field", "Caps the bokeh size (cost grows with it).", 12.0f, 2.0f, 32.0f).visible(on("dof"));
        F("dof.bokehBoost", "Bokeh highlight boost", G, "depth_of_field", "Makes bright out-of-focus points form crisp discs.", 0.5f, 0.0f, 3.0f).visible(on("dof"));

        // Motion blur
        G = "Motion Blur";
        B("mb", "Motion blur", G, "motion_blur", "Smears moving things along their motion like a camera shutter.", false);
        F("mb.shutter", "Shutter angle (deg)", G, "motion_blur", "180 = film standard; 360 = blur over the whole frame time.", 180.0f, 10.0f, 360.0f).visible(on("mb")).fmt("%.0f");
        F("mb.max", "Max blur (px)", G, "motion_blur", "Longest smear allowed.", 32.0f, 4.0f, 96.0f).visible(on("mb"));
        B("mb.cameraOnly", "Camera motion only", G, "motion_blur", "Ignore object motion, blur only from camera movement.", false).visible(on("mb"));

        // Lens & screen
        G = "Lens & Screen";
        B("ca", "Chromatic aberration", G, "chromatic_aberration", "Red/blue color fringes toward the screen edges, like a cheap lens.", false);
        F("ca.intensity", "Aberration intensity", G, "chromatic_aberration", "Strength of the fringes.", 0.5f, 0.0f, 3.0f).visible(on("ca"));
        B("vignette", "Vignette", G, "vignette", "Darkens the screen corners.", true);
        F("vignette.intensity", "Vignette intensity", G, "vignette", "How dark the corners get.", 0.3f, 0.0f, 1.0f).visible(on("vignette"));
        F("vignette.smoothness", "Vignette smoothness", G, "vignette", "Width of the falloff.", 0.4f, 0.01f, 1.0f).visible(on("vignette"));
        F("vignette.roundness", "Vignette roundness", G, "vignette", "1 = circle, 0 = follows the screen rectangle.", 1.0f, 0.0f, 1.0f).visible(on("vignette"));
        B("grain", "Film grain", G, "film_grain", "Animated noise like film or a camera sensor; also hides banding.", false);
        F("grain.intensity", "Grain intensity", G, "film_grain", "Amount of noise.", 0.06f, 0.0f, 0.3f).visible(on("grain")).fmt("%.3f");
        F("grain.size", "Grain size (px)", G, "film_grain", "Size of each grain.", 1.5f, 1.0f, 4.0f).visible(on("grain"));
        F("grain.response", "Luminance response", G, "film_grain", "Reduces grain in highlights like real film.", 0.8f, 0.0f, 1.0f).visible(on("grain"));
        B("grain.colored", "Colored grain", G, "film_grain", "Separate noise per color channel (digital sensor look).", false).visible(on("grain"));
        B("haze", "Heat haze", G, "heat_haze_distortion", "Shimmering distortion over distant hot ground.", false);
        F("haze.strength", "Haze strength", G, "heat_haze_distortion", "Amount of wobble.", 0.6f, 0.0f, 3.0f).visible(on("haze"));
        F("haze.speed", "Haze speed", G, "heat_haze_distortion", "How fast the air shimmers.", 1.0f, 0.0f, 5.0f).visible(on("haze"));
        F("haze.start", "Haze start distance (m)", G, "heat_haze_distortion", "Haze fades in beyond this distance.", 20.0f, 0.0f, 200.0f).visible(on("haze"));
        B("rain", "Rain on lens", G, "screen_rain_wet", "Drops land on the camera lens, refract the scene and slide down.", false);
        F("rain.amount", "Rain amount", G, "screen_rain_wet", "How many drops.", 0.5f, 0.0f, 1.0f).visible(on("rain"));
        F("rain.size", "Drop size", G, "screen_rain_wet", "Size of drops.", 1.0f, 0.3f, 2.0f).visible(on("rain"));
        F("rain.fog", "Lens fog", G, "screen_rain_wet", "Wet, misty blur between drops.", 0.4f, 0.0f, 1.0f).visible(on("rain"));
        B("panini", "Panini projection", G, "panini_projection", "Reduces edge stretching at wide fields of view.", false);
        F("panini.distance", "Panini distance", G, "panini_projection", "0 = normal perspective, 1 = full Panini.", 1.0f, 0.0f, 1.0f).visible(on("panini"));
        F("panini.crop", "Crop to fit", G, "panini_projection", "Zooms in to hide empty corners.", 1.0f, 0.0f, 1.0f).visible(on("panini"));
        E("letterbox", "Letterbox (cinematic bars)", G, "letterbox_cinematic", "Black bars for a wider cinematic aspect ratio.",
          {"Off", "2.39:1 (scope)", "2.00:1", "1.85:1"}, 0).desc.flags = SettingNoPreset;

        // Color grading
        G = "Color Grading";
        B("grade", "Color grading", G, "color_grading_lut", "Artistic color adjustments baked into a 32^3 LUT.", true);
        F("grade.temperature", "White balance: temperature", G, "color_grading_lut", "Negative = cooler/blue, positive = warmer/orange.", 0.0f, -1.0f, 1.0f).visible(on("grade"));
        F("grade.tint", "White balance: tint", G, "color_grading_lut", "Negative = green, positive = magenta.", 0.0f, -1.0f, 1.0f).visible(on("grade"));
        F("grade.contrast", "Contrast", G, "color_grading_lut", "Spreads tones away from middle grey.", 1.0f, 0.5f, 2.0f).visible(on("grade"));
        F("grade.saturation", "Saturation", G, "color_grading_lut", "0 = black and white.", 1.0f, 0.0f, 2.0f).visible(on("grade"));
        F("grade.vibrance", "Vibrance", G, "color_grading_lut", "Boosts dull colors more than already saturated ones.", 0.0f, -1.0f, 1.0f).visible(on("grade"));
        F("grade.hue", "Hue shift", G, "color_grading_lut", "Rotates every hue (-1..1 = -180..180 degrees).", 0.0f, -1.0f, 1.0f).visible(on("grade"));
        F("grade.lift", "Lift (shadows)", G, "color_grading_lut", "Raises or crushes the blacks.", 0.0f, -0.2f, 0.2f).visible(on("grade")).fmt("%.3f");
        F("grade.gamma", "Gamma (midtones)", G, "color_grading_lut", "Brightens or darkens midtones.", 1.0f, 0.5f, 2.0f).visible(on("grade"));
        F("grade.gain", "Gain (highlights)", G, "color_grading_lut", "Scales the highlights.", 1.0f, 0.5f, 2.0f).visible(on("grade"));
        F("grade.split", "Teal & orange split tone", G, "color_grading_lut", "Tints shadows teal and highlights orange (blockbuster look).", 0.0f, 0.0f, 1.0f).visible(on("grade"));
        F("grade.strength", "LUT strength", G, "color_grading_lut", "Blend between ungraded and graded.", 1.0f, 0.0f, 1.0f).visible(on("grade"));
        B("grade.showLut", "Show LUT", G, "color_grading_lut", "Draws the 32 LUT slices in the corner.", false).visible(on("grade")).desc.flags = SettingDebug | SettingNoPreset;

        // HDR calibration (the display.hdr.* values themselves are owned by the core)
        G = "HDR Calibration";
        B("hdrcal", "Show HDR calibration screen", G, "hdr_calibration", "Opens test patterns to set peak brightness, paper white and black level.", false).desc.flags = SettingNoPreset;
    }

    // ------------------------------------------------------------------------------------
    void init(Engine& e) override {
        engine_ = &e;
        auto fs = [&](const char* f, rhi::BlendMode b = rhi::BlendMode::Opaque) { return e.createFullscreenPipeline(std::string("post/") + f + ".frag", {}, b); };
        pHaze_ = fs("heat_haze"); pFocus_ = fs("focus");
        pDofPre_ = fs("dof_prefilter"); pDofBokeh_ = fs("dof_bokeh"); pDofPost_ = fs("dof_postfilter"); pDofComp_ = fs("dof_composite");
        pTile_ = fs("mb_tilemax"); pNeighbor_ = fs("mb_neighbormax"); pGather_ = fs("mb_gather");
        pPanini_ = fs("panini"); pLum_ = fs("luminance"); pAdapt_ = fs("adapt");
        pRaysMask_ = fs("godrays_mask"); pRaysBlur_ = fs("godrays_blur");
        pBloomDown_ = fs("bloom_down"); pBloomUp_ = fs("bloom_up", rhi::BlendMode::Additive);
        pFlare_ = fs("flare"); pRain_ = fs("rain_lens"); pDirt_ = fs("dirt_gen"); pComposite_ = fs("composite_output");
        pTonemap_ = fs("tonemap"); pLut_ = fs("lut_gen"); pLdr_ = fs("postldr");

        auto& dev = e.device();
        params_ = dev.createBuffer({.size = sizeof(PostParams), .usage = rhi::BufUsageUniform, .dynamic = true, .name = "post.params", .owner = name()});
        pass_ = dev.createBuffer({.size = sizeof(PassParams), .usage = rhi::BufUsageUniform, .dynamic = true, .name = "post.pass", .owner = name()});

        rhi::SamplerDesc sd;
        sd.addressU = sd.addressV = sd.addressW = rhi::AddressMode::Clamp;
        sd.mipFilter = rhi::Filter::Nearest;
        mipClamp_ = dev.createSampler(sd);

        auto tex1x1 = [&](const char* n) {
            rhi::TextureDesc d; d.format = rhi::Format::R32F; d.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
            d.name = n; d.owner = name(); return dev.createTexture(d);
        };
        ev_[0] = tex1x1("post.ev0"); ev_[1] = tex1x1("post.ev1");
        focus_[0] = tex1x1("post.focus0"); focus_[1] = tex1x1("post.focus1");

        rhi::TextureDesc ld; ld.type = rhi::TextureType::Tex3D; ld.format = rhi::Format::RGB10A2;
        ld.width = ld.height = ld.depthOrLayers = kLutSize; ld.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
        ld.name = "post.lut"; ld.owner = name();
        lut_ = dev.createTexture(ld);
    }

    void resize(Engine& e, Extent, Extent) override {
        auto& t = e.targets();
        auto desc = [&](rhi::Format f, const char* n, uint32_t mips = 1) {
            rhi::TextureDesc d; d.format = f; d.mipLevels = mips; d.usage = rhi::TexUsageSampled | rhi::TexUsageRenderTarget;
            d.name = n; d.owner = name(); return d;
        };
        dofHalf_  = t.create("post.dof.half",  desc(rhi::Format::RGBA16F, "post.dof.half"),  TargetScale::Render, 0.5f);
        dofBokeh_ = t.create("post.dof.bokeh", desc(rhi::Format::RGBA16F, "post.dof.bokeh"), TargetScale::Render, 0.5f);
        dofPost_  = t.create("post.dof.post",  desc(rhi::Format::RGBA16F, "post.dof.post"),  TargetScale::Render, 0.5f);
        mbTiles_    = t.create("post.mb.tiles",    desc(rhi::Format::RG16F, "post.mb.tiles"),    TargetScale::Render, 1.0f / 16.0f);
        mbNeighbor_ = t.create("post.mb.neighbor", desc(rhi::Format::RG16F, "post.mb.neighbor"), TargetScale::Render, 1.0f / 16.0f);
        lum_ = t.create("post.luminance", desc(rhi::Format::RG16F, "post.luminance", 0), TargetScale::Output, 0.25f);
        raysMask_ = t.create("post.rays.mask", desc(rhi::Format::R11G11B10F, "post.rays.mask"), TargetScale::Output, 0.5f);
        raysA_    = t.create("post.rays.a",    desc(rhi::Format::R11G11B10F, "post.rays.a"),    TargetScale::Output, 0.5f);
        raysB_    = t.create("post.rays.b",    desc(rhi::Format::R11G11B10F, "post.rays.b"),    TargetScale::Output, 0.5f);
        flare_    = t.create("post.flare",     desc(rhi::Format::R11G11B10F, "post.flare"),     TargetScale::Output, 0.5f);
        dirt_     = t.create("post.dirt",      desc(rhi::Format::R8, "post.dirt"),              TargetScale::Output, 0.5f);
        for (int i = 0; i < kMaxBloomMips; i++) {
            std::string n = "post.bloom" + std::to_string(i);
            bloom_[i] = t.create(n, desc(rhi::Format::R11G11B10F, n.c_str()), TargetScale::Output, 1.0f / float(2 << i));
        }
        dirtDirty_ = true; evReset_ = true;
    }

    // ------------------------------------------------------------------------------------
    void addPasses(PassList& p) override {
        auto en = [this](const char* id) { return [this, id] { return master() && s(id)->asBool(); }; };
        // Parameters must be uploaded after core finalizes FrameConstants (FrameSetup order 1000).
        p.add(Stage::FrameSetup, "Post params", [this](FrameContext& ctx) { uploadParams(ctx); }, 2000.0f);

        p.add(Stage::PostRender, "Heat haze",      [this](FrameContext& c) { runHaze(c); },   10.0f, en("haze"));
        p.add(Stage::PostRender, "Depth of field", [this](FrameContext& c) { runDof(c); },    20.0f, en("dof"));
        p.add(Stage::PostRender, "Motion blur",    [this](FrameContext& c) { runMotion(c); }, 30.0f, en("mb"));

        p.add(Stage::PostOutput, "Panini projection", [this](FrameContext& c) { runPanini(c); }, 5.0f, en("panini"));
        p.add(Stage::PostOutput, "Eye adaptation",    [this](FrameContext& c) { runAdapt(c); }, 10.0f, en("exposure.auto"));
        p.add(Stage::PostOutput, "God rays",          [this](FrameContext& c) { runRays(c); },  20.0f, [this] { return master() && s("rays")->asBool() && sunOnScreen_; });
        p.add(Stage::PostOutput, "Bloom downsample",  [this](FrameContext& c) { runBloomDown(c); }, 30.0f, [this] { return needBloomChain(); });
        p.add(Stage::PostOutput, "Lens flare",        [this](FrameContext& c) { runFlare(c); }, 31.0f, en("flare"));
        p.add(Stage::PostOutput, "Bloom upsample",    [this](FrameContext& c) { runBloomUp(c); }, 32.0f,
              [this] { return master() && (s("bloom")->asBool() || s("rain")->asBool()); });
        p.add(Stage::PostOutput, "Rain on lens",      [this](FrameContext& c) { runRain(c); },  40.0f, en("rain"));
        p.add(Stage::PostOutput, "Post composite",    [this](FrameContext& c) { runComposite(c); }, 50.0f,
              [this] { return master() && (s("bloom")->asBool() || s("flare")->asBool() || (s("rays")->asBool() && sunOnScreen_) || s("glare")->asBool()); });

        p.add(Stage::ToneMap, "Tonemap", [this](FrameContext& c) { runTonemap(c); }, 0.0f);

        // After FXAA/SMAA/sharpening (-50/-40) so grain and CA aren't treated as edges.
        p.add(Stage::PostLDR, "Grading, CA, vignette, grain", [this](FrameContext& c) { runLdr(c); }, 0.0f,
              [this] { return (master() && (s("grade")->asBool() || s("ca")->asBool() || s("vignette")->asBool() || s("grain")->asBool()))
                              || letterbox_ > 0.0f || s("hdrcal")->asBool(); });
    }

    void update(FrameContext& ctx) override {
        // Letterbox bars animate in/out (CPU), 0.5 s
        static const float aspects[] = {0.0f, 2.39f, 2.0f, 1.85f};
        float target = aspects[std::clamp(s("letterbox")->asInt(), 0, 3)];
        float screen = float(ctx.outputSize.width) / float(std::max(ctx.outputSize.height, 1u));
        float bar = target > screen ? (1.0f - screen / target) * 0.5f : 0.0f;
        letterbox_ += (bar - letterbox_) * std::min(1.0f, ctx.dt * 8.0f);
        if (std::fabs(bar - letterbox_) < 1e-4f) letterbox_ = bar;
    }

    void onGui(FrameContext& ctx) override {
        if (!s("hdrcal")->asBool()) return;
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("HDR Calibration", &open)) {
            bool hdr = ctx.frame.display2.y > 0.5f;
            ImGui::TextWrapped(hdr ? "HDR output is on. Adjust each slider until the description is true, then Apply."
                                   : "HDR output is off (SDR). Only step 3 (black level / gamma) is meaningful; enable HDR in Display to calibrate peak brightness.");
            ImGui::RadioButton("1. Peak brightness", &calStep_, 0); ImGui::SameLine();
            ImGui::RadioButton("2. Paper white", &calStep_, 1); ImGui::SameLine();
            ImGui::RadioButton("3. Black level", &calStep_, 2);
            if (calStep_ == 0) {
                ImGui::SliderFloat("Logo brightness (nits)", &calPeak_, 300.0f, 4000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
                ImGui::TextWrapped("Raise until the logo just disappears into the background.");
            } else if (calStep_ == 1) {
                ImGui::SliderFloat("Paper white (nits)", &calPaper_, 80.0f, 500.0f, "%.0f");
                ImGui::TextWrapped("Left half = white paper, right = grey card. Pick a comfortable brightness.");
            } else {
                ImGui::SliderFloat("Black level (nits)", &calBlack_, 0.0f, 0.5f, "%.3f");
                ImGui::TextWrapped("Lower until the logo is barely visible.");
            }
            if (ImGui::Button("Apply to display settings")) {
                setIf("display.hdr.peakNits", calPeak_);
                setIf("display.hdr.paperWhiteNits", calPaper_);
                setIf("display.hdr.blackLevelNits", calBlack_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) open = false;
        }
        ImGui::End();
        if (!open) s("hdrcal")->setNumber(0.0f);
    }

    size_t cpuMemoryBytes() const override { return sizeof(*this) + sizeof(PostParams); }

private:
    // ------------------------------------------------------------------------------------
    Setting& add(const char* id, Setting& st) { set_[id] = &st; return st; }
    Setting* s(const char* id) const { return set_.at(id); }
    float f(const char* id) const { return s(id)->asFloat(); }
    float b(const char* id) const { return s(id)->asBool() ? 1.0f : 0.0f; }
    bool master() const { return s("enabled")->asBool(); }
    int quality() const { return std::clamp(s("quality")->asInt(), 0, 3); }
    int q(int lo, int med, int hi, int ultra) const { const int v[] = {lo, med, hi, ultra}; return v[quality()]; }
    bool needBloomChain() const { return master() && (s("bloom")->asBool() || s("flare")->asBool() || s("rain")->asBool()); }
    int bloomMips() const { return q(4, 5, 6, 7); }
    void setIf(const char* id, float v) { if (Setting* st = reg_->find(id)) { st->setNumber(v); reg_->notifyChanged(*st); } }

    void draw(FrameContext& ctx, rhi::Pipeline* pipe, rhi::Texture* target, const char* nm,
              rhi::LoadOp load = rhi::LoadOp::DontCare, uint32_t layer = 0) {
        rhi::RenderPassDesc rp;
        rp.colors[0].texture = target; rp.colors[0].load = load; rp.colors[0].layer = layer;
        rp.colorCount = 1; rp.name = nm;
        ctx.cmd.beginRenderPass(rp);
        ctx.cmd.bindPipeline(pipe);
        ctx.cmd.bindUniformBuffer(0, ctx.frameUBO);
        ctx.cmd.bindUniformBuffer(3, params_.get());
        ctx.cmd.bindUniformBuffer(4, pass_.get());
        ctx.cmd.draw(3);
        ctx.cmd.endRenderPass();
    }
    void passValue(FrameContext& ctx, float x, float y = 0, float z = 0, float w = 0) {
        PassParams pp{glm::vec4(x, y, z, w)};
        ctx.cmd.updateBuffer(pass_.get(), pp);
    }
    void tex(FrameContext& ctx, uint32_t slot, rhi::Texture* t, rhi::Sampler* smp = nullptr) {
        ctx.cmd.bindTexture(slot, t, smp ? smp : engine_->linearClamp());
    }
    rhi::Texture* depth(FrameContext& ctx) { return ctx.targets.get("scene.depth"); }

    // ------------------------------------------------------------------------------------
    void uploadParams(FrameContext& ctx) {
        const FrameConstants& fc = ctx.frame;
        PostParams P{};
        float aspect = float(ctx.outputSize.width) / float(std::max(ctx.outputSize.height, 1u));

        P.haze = {f("haze.strength"), f("haze.speed"), 30.0f, f("haze.start")};
        P.haze2 = {f("haze.start") + 100.0f, 0, 0, 0};

        P.dof = {f("dof.focal"), f("dof.fstop"), 24.0f, f("dof.maxBlur")};
        P.dof2 = {f("dof.focus"), b("dof.autofocus"), f("dof.bokehBoost"), float(q(22, 43, 71, 110))};
        P.dof3 = {s("debug")->asInt() == 4 ? 1.0f : 0.0f, 4.0f, 0, 0};

        bool velocity = ctx.targets.get("scene.velocity") != nullptr;
        P.mb = {f("mb.shutter") / 360.0f, f("mb.max"), float(q(6, 8, 12, 16)), (s("mb.cameraOnly")->asBool() || !velocity) ? 1.0f : 0.0f};

        float tanY = std::tan(fc.cameraParams.z * 0.5f);
        P.panini = {f("panini.distance"), f("panini.crop"), tanY * fc.cameraParams.w, tanY};

        float lumMips = lum_ ? float(lum_->desc().mipLevels > 1 ? lum_->desc().mipLevels - 1
                                     : uint32_t(std::floor(std::log2(float(std::max(lum_->width(), lum_->height())))))) : 0.0f;
        P.exposure = {f("exposure.compensation"), f("exposure.min"), f("exposure.max"), f("exposure.key")};
        P.exposure2 = {f("exposure.speedUp"), f("exposure.speedDown"), f("exposure.center"), (master() && s("exposure.auto")->asBool()) ? 1.0f : 0.0f};
        P.exposure3 = {lumMips, evReset_ ? 1.0f : 0.0f, 0, 0};

        P.rays = {f("rays.density"), f("rays.decay"), f("rays.weight"), float(q(8, 12, 16, 24))};

        // sun position on screen
        glm::vec4 c = fc.unjitteredViewProj * glm::vec4(glm::vec3(fc.sunDirection), 0.0f);
        bool front = c.w > 1e-4f;
        glm::vec2 sunUV = front ? glm::vec2(c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f) : glm::vec2(-10.0f);
        sunOnScreen_ = front && sunUV.x > -0.5f && sunUV.x < 1.5f && sunUV.y > -0.5f && sunUV.y < 1.5f;
        P.sun = {sunUV.x, sunUV.y, front ? 1.0f : 0.0f, aspect};
        P.sunRadiance = glm::vec4(glm::vec3(fc.sunColor) * fc.sunDirection.w, 0.0f);
        P.rays2 = {f("rays.intensity"), 0.06f, (master() && s("rays")->asBool() && sunOnScreen_) ? 1.0f : 0.0f, 0};

        float th = f("bloom.threshold"), knee = th * f("bloom.knee") + 1e-4f;
        P.bloom = {th, th - knee, 2.0f * knee, 0.25f / knee};
        P.bloom2 = {f("bloom.intensity"), f("bloom.radius"), f("bloom.dirt") * f("bloom.intensity") * 4.0f, b("bloom")};
        P.bloomTint = {1, 1, 1, 0};
        P.flare = {f("flare.intensity"), f("flare.spacing"), f("flare.halo"), f("flare.ca")};
        P.flare2 = {float(s("flare.ghosts")->asInt()), b("flare"), 0, 0};
        P.glare = {f("glare.intensity"), f("glare.streaks"), f("glare.size"), f("glare.anamorphic")};
        P.glare2 = {b("glare"), 0, 0, 0};

        P.rain = {f("rain.amount"), f("rain.size"), 0.6f, f("rain.fog")};
        P.rain2 = {float(q(1, 2, 3, 3)), 0, 0, 0};

        bool hdr = fc.display2.y > 0.5f;
        float paper = fc.display.w > 1.0f ? fc.display.w : 200.0f;
        float peak = fc.display2.x > paper ? fc.display2.x : paper;
        P.tonemap = {float(s("tonemap")->asInt()), f("tonemap.white"), hdr ? 1.0f : 0.0f, 0};
        P.display = {paper, peak, peak / paper, 1.0f};

        float split = f("grade.split");
        P.grade = {f("grade.temperature"), f("grade.tint"), f("grade.contrast"), f("grade.saturation")};
        P.grade2 = {f("grade.vibrance"), f("grade.hue") * 3.14159265f, f("grade.strength"), (master() && s("grade")->asBool()) ? 1.0f : 0.0f};
        float lift = f("grade.lift"), gam = f("grade.gamma"), gain = f("grade.gain");
        P.lift = {lift - 0.02f * split, lift + 0.01f * split, lift + 0.03f * split, 0};     // teal shadows
        P.gamma = {gam, gam, gam, 0};
        P.gain = {gain + 0.08f * split, gain + 0.02f * split, gain - 0.08f * split, 0};     // orange highlights

        P.ca = {f("ca.intensity"), float(q(3, 6, 10, 16)), (master() && s("ca")->asBool()) ? 1.0f : 0.0f, 0};
        P.vignette = {f("vignette.intensity"), f("vignette.smoothness"), f("vignette.roundness"), (master() && s("vignette")->asBool()) ? 1.0f : 0.0f};
        P.vignetteColor = {0, 0, 0, 0};
        P.grain = {f("grain.intensity"), f("grain.size"), f("grain.response"), (master() && s("grain")->asBool()) ? 1.0f : 0.0f};
        P.grain2 = {b("grain.colored"), 0, 0, 0};
        P.letterbox = {letterbox_, b("grade.showLut"), float(s("debug")->asInt()), float(kLutSize)};
        P.letterboxColor = {0, 0, 0, 0};
        P.calib = {float(calStep_), calStep_ == 0 ? calPeak_ : calBlack_, b("hdrcal"), 0};
        if (calStep_ == 1) P.display.x = calPaper_;   // preview the paper white being chosen

        ctx.cmd.updateBuffer(params_.get(), P);
        params0_ = P;
    }

    // ------------------------------------------------------------------------------------
    // PostRender
    void runHaze(FrameContext& ctx) {
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, depth(ctx), engine_->pointClamp());
        draw(ctx, pHaze_.get(), ctx.post.dst(), "Heat haze");
        ctx.post.swap();
    }
    void runDof(FrameContext& ctx) {
        rhi::Texture* prev = focus_[focusIdx_].get(); focusIdx_ ^= 1; rhi::Texture* next = focus_[focusIdx_].get();
        tex(ctx, 0, depth(ctx), engine_->pointClamp()); tex(ctx, 1, prev, engine_->pointClamp());
        draw(ctx, pFocus_.get(), next, "DoF auto focus");

        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, depth(ctx), engine_->pointClamp()); tex(ctx, 2, next, engine_->pointClamp());
        draw(ctx, pDofPre_.get(), dofHalf_, "DoF prefilter");
        tex(ctx, 0, dofHalf_);
        draw(ctx, pDofBokeh_.get(), dofBokeh_, "DoF bokeh gather");
        tex(ctx, 0, dofBokeh_);
        draw(ctx, pDofPost_.get(), dofPost_, "DoF postfilter");
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, dofPost_); tex(ctx, 2, dofHalf_);
        draw(ctx, pDofComp_.get(), ctx.post.dst(), "DoF composite");
        ctx.post.swap();
    }
    void runMotion(FrameContext& ctx) {
        rhi::Texture* vel = ctx.targets.get("scene.velocity");
        bool camOnly = params0_.mb.w > 0.5f;
        if (!camOnly) {
            tex(ctx, 0, vel, engine_->pointClamp());
            draw(ctx, pTile_.get(), mbTiles_, "MB tile max");
            tex(ctx, 0, mbTiles_, engine_->pointClamp());
            draw(ctx, pNeighbor_.get(), mbNeighbor_, "MB neighbor max");
        }
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, vel ? vel : depth(ctx), engine_->pointClamp());
        tex(ctx, 2, mbNeighbor_, engine_->pointClamp()); tex(ctx, 3, depth(ctx), engine_->pointClamp());
        draw(ctx, pGather_.get(), ctx.post.dst(), "MB gather");
        ctx.post.swap();
    }

    // ------------------------------------------------------------------------------------
    // PostOutput
    void runPanini(FrameContext& ctx) {
        if (f("panini.distance") < 0.001f) return;
        tex(ctx, 0, ctx.post.src());
        draw(ctx, pPanini_.get(), ctx.post.dst(), "Panini");
        ctx.post.swap();
    }
    void runAdapt(FrameContext& ctx) {
        tex(ctx, 0, ctx.post.src());
        draw(ctx, pLum_.get(), lum_, "Luminance");
        ctx.cmd.generateMips(lum_);
        rhi::Texture* prev = ev_[evIdx_].get(); evIdx_ ^= 1; rhi::Texture* next = ev_[evIdx_].get();
        tex(ctx, 0, lum_, mipClamp_.get()); tex(ctx, 1, prev, engine_->pointClamp());
        draw(ctx, pAdapt_.get(), next, "Adapt exposure");
        if (evReset_) { evReset_ = false; params0_.exposure3.y = 0.0f; ctx.cmd.updateBuffer(params_.get(), params0_); }
    }
    void runRays(FrameContext& ctx) {
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, depth(ctx), engine_->pointClamp());
        draw(ctx, pRaysMask_.get(), raysMask_, "God rays mask");
        float samples = params0_.rays.w;
        passValue(ctx, 1.0f);
        tex(ctx, 0, raysMask_);
        draw(ctx, pRaysBlur_.get(), raysA_, "God rays blur 1");
        passValue(ctx, 1.0f / samples);
        tex(ctx, 0, raysA_);
        draw(ctx, pRaysBlur_.get(), raysB_, "God rays blur 2");
    }
    void runBloomDown(FrameContext& ctx) {
        if (dirtDirty_) { draw(ctx, pDirt_.get(), dirt_, "Lens dirt generate"); dirtDirty_ = false; }
        int n = bloomMips();
        for (int i = 0; i < n; i++) {
            passValue(ctx, i == 0 ? 1.0f : 0.0f);
            tex(ctx, 0, i == 0 ? ctx.post.src() : bloom_[i - 1]);
            draw(ctx, pBloomDown_.get(), bloom_[i], "Bloom down");
        }
    }
    void runFlare(FrameContext& ctx) {
        tex(ctx, 0, bloom_[0]);   // thresholded half-res image (up chain hasn't added into it yet)
        draw(ctx, pFlare_.get(), flare_, "Lens flare");
    }
    void runBloomUp(FrameContext& ctx) {
        int n = bloomMips();
        for (int i = n - 1; i > 0; i--) {
            tex(ctx, 0, bloom_[i]);
            draw(ctx, pBloomUp_.get(), bloom_[i - 1], "Bloom up", rhi::LoadOp::Load);
        }
    }
    void runRain(FrameContext& ctx) {
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, bloom_[std::min(2, bloomMips() - 1)]);
        draw(ctx, pRain_.get(), ctx.post.dst(), "Rain on lens");
        ctx.post.swap();
    }
    void runComposite(FrameContext& ctx) {
        if (dirtDirty_) { draw(ctx, pDirt_.get(), dirt_, "Lens dirt generate"); dirtDirty_ = false; }
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, bloom_[0]); tex(ctx, 2, bloom_[std::max(bloomMips() - 2, 0)]);
        tex(ctx, 3, dirt_); tex(ctx, 4, flare_); tex(ctx, 5, raysB_); tex(ctx, 6, depth(ctx), engine_->pointClamp());
        draw(ctx, pComposite_.get(), ctx.post.dst(), "Post composite");
        ctx.post.swap();
    }

    // ------------------------------------------------------------------------------------
    void runTonemap(FrameContext& ctx) {
        if (!master()) {   // master off: plain ACES, no auto exposure, but keep compensation
            PostParams P = params0_; P.tonemap.x = 4.0f; P.exposure2.w = 0.0f;
            ctx.cmd.updateBuffer(params_.get(), P);
        }
        bool autoOn = master() && s("exposure.auto")->asBool();
        if (!autoOn) evReset_ = true;
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, ev_[evIdx_].get(), engine_->pointClamp());
        draw(ctx, pTonemap_.get(), ctx.post.dst(), "Tonemap");
        ctx.post.swap();
        if (!master()) ctx.cmd.updateBuffer(params_.get(), params0_);
    }

    void buildLut(FrameContext& ctx) {
        const PostParams& P = params0_;
        glm::vec4 key[] = {P.grade, glm::vec4(P.grade2.x, P.grade2.y, 0, 0), P.lift, P.gamma, P.gain};
        uint64_t h = 1469598103934665603ull;
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(key);
        for (size_t i = 0; i < sizeof(key); i++) h = (h ^ bytes[i]) * 1099511628211ull;
        if (lutValid_ && h == lutHash_) return;
        lutHash_ = h; lutValid_ = true;
        for (int z = 0; z < kLutSize; z++) {
            passValue(ctx, float(z), float(kLutSize));
            draw(ctx, pLut_.get(), lut_.get(), "Grading LUT slice", rhi::LoadOp::DontCare, uint32_t(z));
        }
    }
    void runLdr(FrameContext& ctx) {
        if (params0_.grade2.w > 0.5f) buildLut(ctx);
        tex(ctx, 0, ctx.post.src()); tex(ctx, 1, lut_.get());
        draw(ctx, pLdr_.get(), ctx.post.dst(), "Post LDR");
        ctx.post.swap();
    }

    // ------------------------------------------------------------------------------------
    Engine* engine_ = nullptr;
    SettingsRegistry* reg_ = nullptr;
    std::unordered_map<std::string, Setting*> set_;

    rhi::PipelinePtr pHaze_, pFocus_, pDofPre_, pDofBokeh_, pDofPost_, pDofComp_, pTile_, pNeighbor_, pGather_,
        pPanini_, pLum_, pAdapt_, pRaysMask_, pRaysBlur_, pBloomDown_, pBloomUp_, pFlare_, pRain_, pDirt_,
        pComposite_, pTonemap_, pLut_, pLdr_;
    rhi::BufferPtr params_, pass_;
    rhi::SamplerPtr mipClamp_;
    rhi::TexturePtr ev_[2], focus_[2], lut_;
    rhi::Texture *dofHalf_ = nullptr, *dofBokeh_ = nullptr, *dofPost_ = nullptr, *mbTiles_ = nullptr, *mbNeighbor_ = nullptr,
                 *lum_ = nullptr, *raysMask_ = nullptr, *raysA_ = nullptr, *raysB_ = nullptr, *flare_ = nullptr, *dirt_ = nullptr;
    rhi::Texture* bloom_[kMaxBloomMips] = {};

    PostParams params0_{};
    int evIdx_ = 0, focusIdx_ = 0;
    bool evReset_ = true, dirtDirty_ = true, sunOnScreen_ = false, lutValid_ = false;
    uint64_t lutHash_ = 0;
    float letterbox_ = 0.0f;
    int calStep_ = 0;
    float calPeak_ = 1000.0f, calPaper_ = 200.0f, calBlack_ = 0.05f;
};

GFX_REGISTER_MODULE(PostModule);

}  // namespace gfx::post
