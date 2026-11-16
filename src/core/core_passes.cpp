// Core settings and passes: G-buffer, forward/MSAA, transparent meshes, render-scale upscale,
// fallback lighting/tonemap (used only when those modules are absent), output transform.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/engine.h"

namespace gfx {

namespace fs = std::filesystem;

struct CorePassParams {        // std140, UBO binding 2 (core reserved)
    glm::vec4 p0;              // per-pass meaning, see shaders
    glm::vec4 p1;
    glm::ivec4 i0;
};

void Engine::setupCoreSettings() {
    auto& s = settings_;
    // ---------------- Display ----------------
    s.addEnum({.id = "display.windowMode", .label = "Window Mode", .category = "Display", .group = "Display",
               .tooltip = "Windowed, borderless fullscreen (desktop resolution, instant alt-tab) or exclusive fullscreen.",
               .doc = "display_mode", .flags = SettingNoPreset}, {"Windowed", "Borderless", "Fullscreen"}, 0)
        .changed([this](Setting&) { applyWindowMode(); });
    s.addEnum({.id = "display.resolution", .label = "Resolution", .category = "Display", .group = "Display",
               .tooltip = "Output (display) resolution in windowed and exclusive fullscreen modes.",
               .doc = "display_mode", .flags = SettingNoPreset}, {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"}, 1)
        .changed([this](Setting&) { applyWindowMode(); });
    s.addEnum({.id = "display.vsync", .label = "V-Sync", .category = "Display", .group = "Sync",
               .tooltip = "Wait for the display refresh before showing a frame: no tearing, more latency.",
               .doc = "vsync", .flags = SettingNoPreset}, {"Off", "On", "Adaptive", "Half rate"}, 1)
        .changed([this](Setting&) { applyDisplaySettings(); });
    s.addFloat({.id = "display.frameCap", .label = "Frame Rate Limit", .category = "Display", .group = "Sync",
                .tooltip = "Caps the frame rate (0 = unlimited). Steadier frame pacing, lower power and heat.",
                .doc = "frame_rate_limit", .flags = SettingNoPreset}, 0.0f, 0.0f, 360.0f).fmt("%.0f fps");
    s.addFloat({.id = "display.fov", .label = "Field of View", .category = "Display", .group = "Camera",
                .tooltip = "Vertical field of view in degrees. Wider shows more and costs a little more (more objects visible).",
                .doc = "field_of_view", .flags = SettingNoPreset}, 70.0f, 40.0f, 120.0f).fmt("%.0f deg");
    s.addFloat({.id = "input.mouseSensitivity", .label = "Mouse Sensitivity", .category = "Display", .group = "Camera",
                .tooltip = "Fly camera look speed (hold right mouse button, WASD/QE to move, Shift = fast).",
                .flags = SettingNoPreset}, 1.0f, 0.1f, 4.0f);
    // Render resolution
    s.addFloat({.id = "display.renderScale", .label = "Render Resolution", .category = "Display", .group = "Render Resolution",
                .tooltip = "Internal 3D resolution as % of the output. Below 100% renders fewer pixels and upscales; above 100% supersamples.",
                .doc = "render_scale", .flags = SettingRequiresResize}, 100.0f, 25.0f, 200.0f)
        .fmt("%.0f %%").presets(75.0f, 85.0f, 100.0f, 100.0f)
        .enabled([this] { return upscalerRenderScale <= 0.0f; });
    s.addEnum({.id = "display.upscaleFilter", .label = "Upscale Filter", .category = "Display", .group = "Render Resolution",
               .tooltip = "Filter used to stretch the image when render resolution differs from output and no temporal upscaler is active.",
               .doc = "render_scale"}, {"Nearest", "Bilinear", "Catmull-Rom (bicubic)"}, 2)
        .presets(1, 2, 2, 2);
    s.addBool({.id = "display.dynamicRes", .label = "Dynamic Resolution", .category = "Display", .group = "Render Resolution",
               .tooltip = "Automatically lowers/raises render resolution to hold the target frame rate.",
               .doc = "dynamic_resolution", .flags = SettingNoPreset}, false);
    s.addFloat({.id = "display.dynamicRes.targetFps", .label = "Target FPS", .category = "Display", .group = "Render Resolution",
                .doc = "dynamic_resolution", .flags = SettingNoPreset}, 60.0f, 30.0f, 240.0f)
        .fmt("%.0f").visible([this] { return settings_.getBool("display.dynamicRes"); });
    s.addFloat({.id = "display.dynamicRes.min", .label = "Min Scale", .category = "Display", .group = "Render Resolution",
                .doc = "dynamic_resolution", .flags = SettingNoPreset}, 50.0f, 25.0f, 100.0f)
        .fmt("%.0f %%").visible([this] { return settings_.getBool("display.dynamicRes"); });
    s.addFloat({.id = "display.dynamicRes.max", .label = "Max Scale", .category = "Display", .group = "Render Resolution",
                .doc = "dynamic_resolution", .flags = SettingNoPreset}, 100.0f, 50.0f, 200.0f)
        .fmt("%.0f %%").visible([this] { return settings_.getBool("display.dynamicRes"); });
    // Brightness / HDR
    s.addFloat({.id = "display.brightness", .label = "Brightness", .category = "Display", .group = "Brightness & HDR",
                .tooltip = "Scales the final image (applied before gamma). 1.0 = neutral.",
                .doc = "gamma_brightness", .flags = SettingNoPreset}, 1.0f, 0.5f, 1.5f);
    s.addFloat({.id = "display.gamma", .label = "Gamma", .category = "Display", .group = "Brightness & HDR",
                .tooltip = "Display gamma used to encode the SDR image. 2.2 matches most monitors; 'sRGB' curve at exactly 2.2.",
                .doc = "gamma_brightness", .flags = SettingNoPreset}, 2.2f, 1.6f, 2.8f);
    s.addEnum({.id = "display.hdr.mode", .label = "HDR Output", .category = "Display", .group = "Brightness & HDR",
               .tooltip = "SDR, HDR10 (PQ, Rec.2020) or scRGB (linear FP16). Needs an HDR display and swapchain; otherwise previewed.",
               .doc = "hdr_output", .flags = SettingNoPreset}, {"SDR", "HDR10 (PQ)", "scRGB (FP16)"}, 0)
        .changed([this](Setting&) { applyDisplaySettings(); });
    auto hdrOn = [this] { return settings_.getInt("display.hdr.mode") != 0; };
    s.addFloat({.id = "display.hdr.peakNits", .label = "HDR Peak Brightness", .category = "Display", .group = "Brightness & HDR",
                .doc = "hdr_output", .flags = SettingNoPreset}, 1000.0f, 400.0f, 10000.0f).fmt("%.0f nits").visible(hdrOn);
    s.addFloat({.id = "display.hdr.paperWhiteNits", .label = "HDR Paper White", .category = "Display", .group = "Brightness & HDR",
                .doc = "hdr_output", .flags = SettingNoPreset}, 203.0f, 80.0f, 500.0f).fmt("%.0f nits").visible(hdrOn);
    s.addFloat({.id = "display.hdr.blackLevelNits", .label = "HDR Black Level", .category = "Display", .group = "Brightness & HDR",
                .doc = "hdr_output", .flags = SettingNoPreset}, 0.0f, 0.0f, 0.5f).fmt("%.3f nits").visible(hdrOn);

    // ---------------- Latency ----------------
    s.addEnum({.id = "latency.mode", .label = "Low Latency Mode", .category = "Latency", .group = "Latency",
               .tooltip = "Reflex/Anti-Lag style: limit the render queue to 1 frame and sample input just before rendering. Boost also drains the GPU each frame.",
               .doc = "low_latency", .flags = SettingNoPreset}, {"Off", "On", "On + Boost"}, 0);
    s.addInt({.id = "latency.maxFramesInFlight", .label = "Max Frames in Flight", .category = "Latency", .group = "Latency",
              .tooltip = "How many frames the CPU may queue ahead of the GPU (render-ahead). More = smoother under load, more input lag.",
              .doc = "low_latency", .flags = SettingNoPreset}, 2, 1, 4)
        .enabled([this] { return settings_.getInt("latency.mode") == 0; });

    // ---------------- Graphics ----------------
    sceneFiles_.clear();
    std::vector<std::string> sceneNames = {"Procedural demo"};
    fs::path assets = fs::path(GFX_SOURCE_DIR) / "assets";
    std::error_code ec;
    if (fs::exists(assets, ec))
        for (auto& e : fs::recursive_directory_iterator(assets, ec))
            if (e.path().extension() == ".gltf" || e.path().extension() == ".glb") {
                sceneFiles_.push_back(e.path().string());
                sceneNames.push_back(e.path().stem().string());
            }
    s.addEnum({.id = "scene.select", .label = "Scene", .category = "Graphics", .group = "Scene",
               .tooltip = "Procedural demo or any glTF found under assets/.", .doc = "model_loading",
               .flags = SettingNoPreset}, sceneNames, 0)
        .changed([this](Setting& st) { pendingSceneLoad_ = st.asInt(); });

    // ---------------- Debug ----------------
    s.addEnum({.id = "debug.overlay", .label = "Performance Overlay", .category = "Debug", .group = "Overlay",
               .tooltip = "FPS / frame time graph / per-pass GPU+CPU time / RAM / VRAM.", .doc = "performance_overlay",
               .flags = SettingNoPreset}, {"Off", "FPS", "Detailed"}, 2);
    s.addEnum({.id = "debug.view", .label = "Debug View", .category = "Debug", .group = "Buffers",
               .tooltip = "Show an intermediate buffer instead of the final image.", .doc = "performance_overlay",
               .flags = SettingNoPreset},
              {"Final", "Albedo", "Normal", "Roughness", "Metallic", "Material AO", "Depth", "Velocity", "Emissive", "Scene HDR (pre-post)"}, 0);
    s.addBool({.id = "shaders.cache", .label = "Shader Cache", .category = "Graphics", .group = "Shaders",
               .tooltip = "Store compiled shader binaries on disk and reuse them: faster startup and no recompile hitches on the next run.",
               .doc = "shader_cache_precompilation", .flags = SettingNoPreset | SettingRequiresReload}, true)
        .changed([this](Setting& st) { device_->setProgramCacheDir(st.asBool() ? "shader_cache" : ""); });
    s.addBool({.id = "shaders.hotReload", .label = "Shader Hot Reload", .category = "Debug", .group = "Shaders",
               .tooltip = "Watch shaders/ and recompile changed programs live (F5 forces a full reload).", .doc = "shader_hot_reload",
               .flags = SettingNoPreset}, true);
}

// ---------------------------------------------------------------------------------------
void Engine::addCorePasses() {
    auto& dev = *device_;
    auto params = dev.createBuffer({.size = sizeof(CorePassParams), .usage = rhi::BufUsageUniform, .name = "core pass params", .owner = "core"});
    rhi::Buffer* paramsBuf = params.get();
    coreBuffers_.push_back(std::move(params));

    // ---- FrameSetup: default draw list, then culling/LOD modules ----
    passes_.add(Stage::FrameSetup, "Draw list", [this](FrameContext& ctx) {
        renderer_.buildDefaultDrawList(ctx);
        for (auto& m : modules_) m->filterDraws(ctx, ctx.draws);
        if (pendingSceneLoad_ >= 0) { /* handled at frame start */ }
    }, -100.0f);
    passes_.add(Stage::FrameSetup, "Frame constants", [this](FrameContext& ctx) { fillFrameConstants(ctx); }, 1000.0f);

    // ---- Sky pipelines (forward color+velocity, G-buffer velocity only) ----
    {
        rhi::PipelineDesc d;
        d.program = fullscreenProgram("core/sky.frag", {"SKY_VELOCITY_ONLY=1"});
        d.raster.cull = rhi::CullMode::None;
        d.depth.test = false; d.depth.write = false;
        for (int i = 0; i < 4; ++i) d.blend[i].writeMask = 0;
        corePipelines_.push_back(device_->createPipeline(d));
        d.program = fullscreenProgram("core/sky.frag");
        for (int i = 0; i < 4; ++i) d.blend[i].writeMask = 0xF;
        corePipelines_.push_back(device_->createPipeline(d));
    }
    rhi::Pipeline* skyVelocityP = corePipelines_[corePipelines_.size() - 2].get();
    rhi::Pipeline* skyForwardP = corePipelines_.back().get();

    // ---- GBuffer ----
    passes_.add(Stage::GBuffer, "G-Buffer", [this, skyVelocityP](FrameContext& ctx) {
        rhi::RenderPassDesc rp;
        rp.name = "G-Buffer";
        const char* names[] = {"gbuffer.albedo", "gbuffer.normal", "gbuffer.material", "gbuffer.emissive", "scene.velocity"};
        rp.colorCount = 5;
        for (int i = 0; i < 5; ++i) { rp.colors[i].texture = ctx.targets.get(names[i]); rp.colors[i].load = rhi::LoadOp::Clear; }
        rp.colors[2].clear[0] = 1.0f;   // roughness 1 for sky
        rp.depth.texture = ctx.targets.get("scene.depth");
        rp.depth.load = rhi::LoadOp::Clear;
        rp.depth.clearDepth = 0.0f;
        ctx.cmd.beginRenderPass(rp);
        ctx.cmd.bindPipeline(skyVelocityP);      // sky motion vectors (camera rotation) before geometry
        ctx.cmd.draw(3);
        SceneDrawParams p;
        p.mode = DrawMode::GBuffer;
        p.viewProj = ctx.frame.viewProj;
        p.prevViewProj = ctx.frame.prevViewProj;
        p.mipBias = ctx.frame.display2.z;
        ctx.renderer.drawScene(ctx, p);
        ctx.cmd.endRenderPass();
    }, 0.0f, [this] { return msaaSamples_ <= 1; });

    // ---- Lighting fallback (only when no lighting module is compiled in) ----
    auto deferred = createFullscreenPipeline("core/deferred_fallback.frag");
    rhi::Pipeline* deferredP = deferred.get();
    corePipelines_.push_back(std::move(deferred));
    passes_.add(Stage::Lighting, "Deferred lighting (core fallback)", [this, deferredP](FrameContext& ctx) {
        if (ctx.replaceLighting) return;
        auto* pc = pointClamp();
        ctx.cmd.bindTexture(0, ctx.targets.get("gbuffer.albedo"), pc);
        ctx.cmd.bindTexture(1, ctx.targets.get("gbuffer.normal"), pc);
        ctx.cmd.bindTexture(2, ctx.targets.get("gbuffer.material"), pc);
        ctx.cmd.bindTexture(3, ctx.targets.get("gbuffer.emissive"), pc);
        ctx.cmd.bindTexture(4, ctx.targets.get("scene.depth"), pc);
        ctx.cmd.bindStorageBuffer(0, ctx.scene.lightBuffer.get());
        ctx.fullscreen(deferredP, ctx.targets.get("scene.color"), "Deferred lighting");
    }, 100.0f, [this] { return !hasLightingModule_ && msaaSamples_ <= 1; });

    // ---- Forward (MSAA) ----
    passes_.add(Stage::Forward, "Forward + MSAA", [this, skyForwardP](FrameContext& ctx) {
        for (auto& m : modules_) m->bindShadingResources(ctx);
        rhi::RenderPassDesc rp;
        rp.name = "Forward MSAA";
        rp.colorCount = 2;
        rp.colors[0].texture = ctx.targets.get("msaa.color"); rp.colors[0].load = rhi::LoadOp::Clear;
        rp.colors[1].texture = ctx.targets.get("msaa.velocity"); rp.colors[1].load = rhi::LoadOp::Clear;
        rp.depth.texture = ctx.targets.get("msaa.depth"); rp.depth.load = rhi::LoadOp::Clear;
        ctx.cmd.beginRenderPass(rp);
        if (!ctx.replaceLighting) { ctx.cmd.bindPipeline(skyForwardP); ctx.cmd.draw(3); }
        SceneDrawParams p;
        p.mode = DrawMode::Forward;
        p.viewProj = ctx.frame.viewProj;
        p.prevViewProj = ctx.frame.prevViewProj;
        p.mipBias = ctx.frame.display2.z;
        ctx.renderer.drawScene(ctx, p);
        ctx.cmd.endRenderPass();
        ctx.cmd.resolveTexture(ctx.targets.get("msaa.color"), ctx.targets.get("scene.color"));
        ctx.cmd.resolveTexture(ctx.targets.get("msaa.velocity"), ctx.targets.get("scene.velocity"));
        ctx.cmd.resolveTexture(ctx.targets.get("msaa.depth"), ctx.targets.get("scene.depth"));
    }, 0.0f, [this] { return msaaSamples_ > 1; });

    // ---- Transparent meshes ----
    passes_.add(Stage::Translucent, "Transparent meshes", [this](FrameContext& ctx) {
        if (ctx.draws.transparent.empty()) return;
        for (auto& m : modules_) m->bindShadingResources(ctx);
        rhi::RenderPassDesc rp;
        rp.name = "Transparent";
        rp.colorCount = 1;
        rp.colors[0].texture = ctx.targets.get("scene.color");
        rp.depth.texture = ctx.targets.get("scene.depth");
        rp.depth.readOnly = true;
        ctx.cmd.beginRenderPass(rp);
        SceneDrawParams p;
        p.mode = DrawMode::Forward;
        p.transparent = true;
        p.viewProj = ctx.frame.viewProj;
        p.prevViewProj = ctx.frame.prevViewProj;
        ctx.renderer.drawScene(ctx, p);
        ctx.cmd.endRenderPass();
    }, 0.0f);

    // ---- Upscale (render scale) when no temporal upscaler swapped the chain ----
    for (int f = 0; f < 3; ++f) upscalePipeline_[f] = createFullscreenPipeline("core/upscale.frag", {"FILTER=" + std::to_string(f)});
    passes_.add(Stage::Upscale, "Upscale (render scale)", [this, paramsBuf](FrameContext& ctx) {
        if (ctx.post.upscaled()) return;
        int filter = std::clamp(settings_.getInt("display.upscaleFilter"), 0, 2);
        rhi::Texture* src = ctx.post.src();
        rhi::Texture* dst = ctx.post.dst();
        CorePassParams cp{};
        cp.p0 = glm::vec4(src->width(), src->height(), 1.0f / src->width(), 1.0f / src->height());
        ctx.cmd.updateBuffer(paramsBuf, cp);
        ctx.cmd.bindUniformBuffer(2, paramsBuf);
        ctx.cmd.bindTexture(0, src, filter == 0 ? pointClamp() : linearClamp());
        ctx.fullscreen(upscalePipeline_[filter].get(), dst, "Upscale");
        ctx.post.swap();
    }, 1000.0f);

    // ---- Tone map fallback ----
    auto tonemap = createFullscreenPipeline("core/tonemap_fallback.frag");
    rhi::Pipeline* tonemapP = tonemap.get();
    corePipelines_.push_back(std::move(tonemap));
    passes_.add(Stage::ToneMap, "Tone map (core fallback)", [this, tonemapP](FrameContext& ctx) {
        ctx.cmd.bindTexture(0, ctx.post.src(), pointClamp());
        ctx.fullscreen(tonemapP, ctx.post.dst(), "Tonemap");
        ctx.post.swap();
    }, 0.0f, [this] { return !hasToneMapModule_; });

    // ---- Output transform: brightness, gamma / PQ / scRGB, debug views ----
    auto output = createFullscreenPipeline("core/output_transform.frag");
    rhi::Pipeline* outputP = output.get();
    corePipelines_.push_back(std::move(output));
    passes_.add(Stage::OutputTransform, "Output transform", [this, outputP, paramsBuf](FrameContext& ctx) {
        CorePassParams cp{};
        int view = settings_.getInt("debug.view");
        cp.i0 = glm::ivec4(view, hdrSwapchainActive_ ? 1 : 0, 0, 0);
        ctx.cmd.updateBuffer(paramsBuf, cp);
        ctx.cmd.bindUniformBuffer(2, paramsBuf);
        auto* pc = pointClamp();
        ctx.cmd.bindTexture(0, ctx.post.src(), pc);
        ctx.cmd.bindTexture(1, ctx.targets.get("gbuffer.albedo"), linearClamp());
        ctx.cmd.bindTexture(2, ctx.targets.get("gbuffer.normal"), linearClamp());
        ctx.cmd.bindTexture(3, ctx.targets.get("gbuffer.material"), linearClamp());
        ctx.cmd.bindTexture(4, ctx.targets.get("scene.depth"), pc);
        ctx.cmd.bindTexture(5, ctx.targets.get("scene.velocity"), linearClamp());
        ctx.cmd.bindTexture(6, ctx.targets.get("gbuffer.emissive"), linearClamp());
        ctx.cmd.bindTexture(7, ctx.targets.get("scene.color"), linearClamp());
        ctx.fullscreen(outputP, ctx.targets.get("final.output"), "Output transform");
    }, 0.0f);
}

}  // namespace gfx
