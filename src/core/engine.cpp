#include "core/engine.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <glm/gtc/matrix_transform.hpp>

#include "core/gui.h"
#include "rhi/backends.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace gfx {

using Clock = std::chrono::high_resolution_clock;
namespace fs = std::filesystem;

static double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// Sleep most of the interval, spin the rest (OS sleep granularity is ~1 ms on Windows, worse on some Linux VMs).
static void preciseWaitUntil(double target) {
    for (;;) {
        double remaining = target - nowSec();
        if (remaining <= 0.0) return;
        if (remaining > 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.0015));
        else std::this_thread::yield();
    }
}

Engine::Engine() = default;
Engine::~Engine() { shutdown(); }

Module* Engine::findModule(const std::string& name) {
    for (auto& m : modules_) if (name == m->name()) return m.get();
    return nullptr;
}

rhi::Program* Engine::fullscreenProgram(const std::string& fsFile, std::vector<std::string> defines) {
    return shaders_.graphics("common/fullscreen.vert", fsFile, std::move(defines));
}

rhi::PipelinePtr Engine::createFullscreenPipeline(const std::string& fsFile, std::vector<std::string> defines, rhi::BlendMode blend) {
    rhi::PipelineDesc d;
    d.program = fullscreenProgram(fsFile, std::move(defines));
    d.raster.cull = rhi::CullMode::None;
    d.depth.test = false;
    d.depth.write = false;
    d.blend[0].mode = blend;
    d.name = fsFile;
    return device_->createPipeline(d);
}

// ---------------------------------------------------------------------------------------
bool Engine::init(const EngineConfig& cfg) {
    cfg_ = cfg;
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return false; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
    if (cfg.hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window_ = glfwCreateWindow(int(cfg.width), int(cfg.height), "gfx-settings-lab", nullptr, nullptr);
    if (!window_) { std::fprintf(stderr, "Could not create an OpenGL 4.5 core window\n"); return false; }
    glfwMakeContextCurrent(window_);
    if (!gladLoadGL(glfwGetProcAddress)) { std::fprintf(stderr, "gladLoadGL failed\n"); return false; }
    glfwSetWindowUserPointer(window_, this);
    glfwSetWindowFocusCallback(window_, [](GLFWwindow* w, int f) { static_cast<Engine*>(glfwGetWindowUserPointer(w))->focused_ = f != 0; });

    if (cfg.backend == rhi::Backend::D3D11) {
#if defined(GFX_HAS_D3D11)
        std::fprintf(stderr, "[engine] D3D11 backend requested: see src/rhi/d3d11 (GL context used for this build)\n");
#else
        std::fprintf(stderr, "[engine] D3D11 backend not built in this configuration, using OpenGL\n");
#endif
    }
    device_ = rhi::createGLDevice(window_);
    const auto& caps = device_->caps();
    std::printf("[engine] %s %s | %s | %s\n", caps.apiName.c_str(), caps.version.c_str(), caps.renderer.c_str(), caps.vendor.c_str());

    fs::path shaderRoot = cfg.shaderRoot.empty() ? fs::path(GFX_SOURCE_DIR) / "shaders" : fs::path(cfg.shaderRoot);
    if (!fs::exists(shaderRoot) && fs::exists("shaders")) shaderRoot = "shaders";
    shaders_.init(*device_, shaderRoot);
    targets_ = std::make_unique<RenderTargets>(*device_);
    profiler_.init(*device_);

    rhi::SamplerDesc s;
    s.addressU = s.addressV = s.addressW = rhi::AddressMode::Clamp;
    s.useMips = false;
    linearClamp_ = device_->createSampler(s);
    s.minFilter = s.magFilter = rhi::Filter::Nearest;
    pointClamp_ = device_->createSampler(s);
    rhi::SamplerDesc r;
    linearRepeat_ = device_->createSampler(r);

    rhi::BufferDesc fb;
    fb.size = sizeof(FrameConstants);
    fb.usage = rhi::BufUsageUniform;
    fb.name = "frame constants";
    frameUBO_ = device_->createBuffer(fb);

    // Modules
    for (auto& f : moduleFactories()) modules_.push_back(f());
    std::stable_sort(modules_.begin(), modules_.end(), [](auto& a, auto& b) { return a->priority() < b->priority(); });
    setupCoreSettings();
    for (auto& m : modules_) m->registerSettings(settings_);
    settings_.onAnyChange = [this](Setting& st, float oldValue) {
        profiler_.onSettingChanged(st, oldValue);
        if (!(st.desc.flags & SettingNoPreset) && !applyingPreset_) settings_.markCustom();
    };
    settings_.load(cfg_.settingsPath);
    device_->setProgramCacheDir(settings_.getBool("shaders.cache") ? "shader_cache" : "");

    // Scene
    std::string err;
    std::string scenePath = cfg.scenePath;
    if (scenePath.empty()) {
        int sel = settings_.getInt("scene.select");
        if (sel > 0 && sel - 1 < int(sceneFiles_.size())) scenePath = sceneFiles_[sel - 1];
    }
    if (scenePath.empty() || !scene_.loadGltf(scenePath, *device_, &err)) {
        if (!scenePath.empty()) std::fprintf(stderr, "[scene] %s, using the procedural demo\n", err.c_str());
        scene_.buildProceduralDemo(*device_);
    }
    if (scene_.bounds.valid() && scene_.sourcePath != "procedural") {
        glm::vec3 c = scene_.bounds.center(), e = scene_.bounds.extent();
        camera_.position = c + glm::vec3(0, e.y * 0.2f, 0);
        camera_.yaw = 0.0f; camera_.pitch = 0.0f;
    }

    renderer_.init(*this);
    gui_ = std::make_unique<CoreGui>(*this);
    gui_->init();
    if (cfg.maxFrames > 0 || !cfg.benchSettingsDir.empty()) gui_->showMenu = false;   // clean screenshots

    int fw = 0, fh = 0;
    glfwGetFramebufferSize(window_, &fw, &fh);
    outputSize_ = {uint32_t(std::max(1, fw)), uint32_t(std::max(1, fh))};
    device_->resizeSwapchain(outputSize_.width, outputSize_.height);
    updateRenderSize();
    createCoreTargets();
    targets_->resize(renderSize_, outputSize_);

    for (auto& m : modules_) m->init(*this);
    for (auto& m : modules_) m->resize(*this, renderSize_, outputSize_);

    passes_.currentOwner = "core";
    addCorePasses();
    for (auto& m : modules_) { passes_.currentOwner = m->name(); m->addPasses(passes_); }
    passes_.currentOwner.clear();
    passes_.sort();
    for (auto& p : passes_.passes()) {
        if (p.owner == "core") continue;
        // The deferred resolve belongs to the lighting area; RT/GI passes in Stage::Lighting don't replace it.
        if (p.stage == Stage::Lighting && p.owner.rfind("lighting", 0) == 0) hasLightingModule_ = true;
        if (p.stage == Stage::ToneMap) hasToneMapModule_ = true;
    }
    { auto cs = device_->shaderCacheStats(); std::printf("[engine] shaders: %zu programs, %.0f ms compile, cache %u hits / %u misses\n", shaders_.programCount(), cs.compileMs, cs.hits, cs.misses); }
    std::printf("[engine] %zu modules, %zu passes, %zu settings\n", modules_.size(), passes_.passes().size(), settings_.all().size());
    for (auto& m : modules_) std::printf("  module %s\n", m->name());

    applyDisplaySettings();
    lastTime_ = nowSec();
    return true;
}

// ---------------------------------------------------------------------------------------
void Engine::updateRenderSize() {
    float scale = upscalerRenderScale > 0.0f ? upscalerRenderScale : settings_.getFloat("display.renderScale") / 100.0f;
    if (settings_.getBool("display.dynamicRes")) scale *= dynamicScale_;
    scale = std::clamp(scale, 0.25f, 2.0f);
    renderScale = scale;
    renderSize_ = {std::max(1u, uint32_t(outputSize_.width * scale + 0.5f)), std::max(1u, uint32_t(outputSize_.height * scale + 0.5f))};
}

void Engine::createCoreTargets() {
    using namespace rhi;
    auto rt = [&](const char* name, Format f, TargetScale ts, uint32_t usage = TexUsageSampled | TexUsageRenderTarget, uint32_t samples = 1) {
        TextureDesc d;
        d.format = f; d.usage = usage; d.samples = samples; d.owner = "core.targets";
        targets_->create(name, d, ts);
    };
    rt("scene.color", Format::RGBA16F, TargetScale::Render, TexUsageSampled | TexUsageRenderTarget | TexUsageStorage);
    rt("scene.depth", Format::D32F, TargetScale::Render, TexUsageSampled | TexUsageDepthStencil);
    rt("scene.velocity", Format::RG16F, TargetScale::Render);
    rt("gbuffer.albedo", Format::RGBA8_SRGB, TargetScale::Render);
    rt("gbuffer.normal", Format::RGBA16F, TargetScale::Render);
    rt("gbuffer.material", Format::RGBA8, TargetScale::Render);
    rt("gbuffer.emissive", Format::RGBA16F, TargetScale::Render);
    rt("post.renderScratch", Format::RGBA16F, TargetScale::Render, TexUsageSampled | TexUsageRenderTarget | TexUsageStorage);
    rt("post.outputA", Format::RGBA16F, TargetScale::Output, TexUsageSampled | TexUsageRenderTarget | TexUsageStorage);
    rt("post.outputB", Format::RGBA16F, TargetScale::Output, TexUsageSampled | TexUsageRenderTarget | TexUsageStorage);
    rt("final.output", Format::RGBA16F, TargetScale::Output, TexUsageSampled | TexUsageRenderTarget | TexUsageStorage);

    int msaa = 1 << std::clamp(settings_.getInt("aa.msaa.samples"), 0, 3);
    msaa = std::min<int>(msaa, int(device_->caps().maxSamples));
    msaaSamples_ = msaa;
    if (msaa > 1) {
        rt("msaa.color", Format::RGBA16F, TargetScale::Render, TexUsageRenderTarget, uint32_t(msaa));
        rt("msaa.velocity", Format::RG16F, TargetScale::Render, TexUsageRenderTarget, uint32_t(msaa));
        rt("msaa.depth", Format::D32F, TargetScale::Render, TexUsageDepthStencil, uint32_t(msaa));
    } else {
        targets_->remove("msaa.color"); targets_->remove("msaa.velocity"); targets_->remove("msaa.depth");
    }
}

void Engine::handleResize() {
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(window_, &fw, &fh);
    while ((fw == 0 || fh == 0) && !glfwWindowShouldClose(window_)) { glfwWaitEvents(); glfwGetFramebufferSize(window_, &fw, &fh); }
    Extent out{uint32_t(fw), uint32_t(fh)};
    Extent prevRender = renderSize_, prevOut = outputSize_;
    outputSize_ = out;
    updateRenderSize();
    bool settingsResize = settings_.consumeResizeRequest() || requestResize;
    requestResize = false;
    if (!settingsResize && renderSize_ == prevRender && outputSize_ == prevOut) return;
    device_->waitIdle();
    device_->resizeSwapchain(outputSize_.width, outputSize_.height);
    createCoreTargets();
    targets_->resize(renderSize_, outputSize_);
    for (auto& m : modules_) m->resize(*this, renderSize_, outputSize_);
}

// ---------------------------------------------------------------------------------------
void Engine::runStage(FrameContext& ctx, Stage s) {
    if (s == Stage::Upscale) ctx.post.enterOutputRes();
    for (auto& p : passes_.passes()) {
        if (p.stage != s) continue;
        p.ranThisFrame = false;
        if (p.enabled && !p.enabled()) continue;
        std::string key = p.owner + "/" + p.name;
        ctx.cmd.pushDebugGroup(p.name.c_str());
        auto* t = profiler_.beginPass(key);
        ctx.cmd.bindUniformBuffer(0, frameUBO_.get());
        p.execute(ctx);
        profiler_.endPass(t);
        ctx.cmd.popDebugGroup();
        p.ranThisFrame = true;
        p.gpuMs = profiler_.passGpuMs(key);
        p.cpuMs = profiler_.passCpuMs(key);
    }
}

void Engine::frame() {
    double waitMs = 0.0;
    auto waitStart = Clock::now();

    // ---- Latency: frame queue depth + frame cap (see docs/latency_sync.md) ----
    int latencyMode = settings_.getInt("latency.mode");   // 0 Off, 1 On, 2 On + Boost
    device_->setMaxFrameLatency(latencyMode > 0 ? 1u : uint32_t(settings_.getInt("latency.maxFramesInFlight")));
    device_->waitForFrameLatency();
    float cap = settings_.getFloat("display.frameCap");
    if (cap > 0.0f) {
        double target = lastFrameStart_ + 1.0 / cap;
        preciseWaitUntil(target);
        lastFrameStart_ = std::max(target, nowSec() - 0.25);
    } else {
        lastFrameStart_ = nowSec();
    }
    if (latencyMode == 2) device_->waitIdle();   // Boost: GPU fully drained, CPU starts on fresh input
    waitMs = std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count();

    // Input is sampled AFTER the wait (just-in-time), which is what makes the low-latency mode work.
    glfwPollEvents();
    double t = nowSec();
    float dt = float(std::min(0.1, t - lastTime_));
    lastTime_ = t;
    time_ += dt;
    processInput(dt);

    if (settings_.consumeReloadRequest()) shaders_.reloadAll();
    if (settings_.getBool("shaders.hotReload") && frameIndex_ % 30 == 0) shaders_.checkForChanges();
    if (pendingSceneLoad_ >= 0) {
        int sel = pendingSceneLoad_;
        pendingSceneLoad_ = -1;
        device_->waitIdle();
        std::string err;
        if (sel == 0 || sel - 1 >= int(sceneFiles_.size()) || !scene_.loadGltf(sceneFiles_[sel - 1], *device_, &err)) scene_.buildProceduralDemo(*device_);
        for (auto& m : modules_) m->resize(*this, renderSize_, outputSize_);
    }
    handleResize();

    profiler_.beginFrame();
    renderer_.beginFrame();
    auto& cmd = device_->cmd();

    FrameContext ctx{*this, *device_, cmd, settings_, scene_, renderer_, shaders_, *targets_, post_, profiler_, camera_, draws_};
    ctx.frameUBO = frameUBO_.get();
    ctx.renderSize = renderSize_;
    ctx.outputSize = outputSize_;
    ctx.frameIndex = frameIndex_;
    ctx.dt = dt;
    ctx.time = float(time_);
    ctx.msaaForward = msaaSamples_ > 1;
    camera_.fovY = glm::radians(settings_.getFloat("display.fov"));
    camera_.prevJitterPixels = camera_.jitterPixels;
    camera_.jitterPixels = glm::vec2(0.0f);
    fillFrameConstants(ctx);   // preliminary (unjittered) so update()/FrameSetup can read matrices

    for (auto& m : modules_) m->update(ctx);
    scene_.upload(*device_);
    post_.beginFrame(targets_->get("scene.color"), targets_->get("post.renderScratch"), targets_->get("post.outputA"), targets_->get("post.outputB"));
    curCtx_ = &ctx;

    for (int s = 0; s < int(Stage::Count); ++s) {
        if (Stage(s) == Stage::UI) break;   // UI handled after present-copy
        runStage(ctx, Stage(s));
    }

    // ---- Present (frame generation may insert an interpolated frame first) ----
    rhi::Texture* finalTex = targets_->get("final.output");
    if (ctx.interpolatedFrame) {
        device_->copyToBackbuffer(ctx.interpolatedFrame);
        device_->beginUiOverlay();
        gui_->render(ctx, false);
        device_->endUiOverlay();
        device_->swapBuffers();
        // Pace: show the generated frame for half of the last real frame interval.
        double half = profiler_.average(10).frameMs * 0.5e-3;
        preciseWaitUntil(nowSec() + std::min(half, 0.05));
        ++presentedFrames_;
    }
    device_->copyToBackbuffer(finalTex);
    device_->beginUiOverlay();
    runStage(ctx, Stage::UI);
    gui_->render(ctx, true);
    device_->endUiOverlay();
    if (!pendingScreenshot_.empty()) { writeScreenshot(pendingScreenshot_); pendingScreenshot_.clear(); }
    device_->swapBuffers();
    ++presentedFrames_;

    profiler_.counter("draw calls", renderer_.drawCallsThisFrame);
    {
        auto cs = device_->shaderCacheStats();
        profiler_.counter("shaders.compileMsTotal", cs.compileMs);
        profiler_.counter("shaders.cacheHits", cs.hits);
        profiler_.counter("shaders.cacheMisses", cs.misses);
        profiler_.counter("shaders.programs", double(shaders_.programCount()));
    }
    profiler_.counter("triangles", renderer_.trianglesThisFrame);
    double offThread = 0.0;
    for (auto& m : modules_) offThread += m->offThreadCpuMs();
    profiler_.endFrame(waitMs, offThread);
    updateDynamicResolution();

    prevFrame_ = ctx.frame;
    for (auto& inst : scene_.instances) inst.prevWorld = inst.world;
    curCtx_ = nullptr;
    ++frameIndex_;
}

void Engine::fillFrameConstants(FrameContext& ctx) {
    FrameConstants& f = ctx.frame;
    glm::mat4 view = camera_.viewMatrix();
    glm::mat4 unj = camera_.projection(renderSize_);
    glm::mat4 proj = camera_.projection(renderSize_, camera_.jitterPixels);
    f.view = view;
    f.proj = proj;
    f.viewProj = proj * view;
    f.invView = glm::inverse(view);
    f.invProj = glm::inverse(proj);
    f.invViewProj = glm::inverse(f.viewProj);
    f.unjitteredProj = unj;
    f.unjitteredViewProj = unj * view;
    f.prevViewProj = frameIndex_ == 0 ? f.unjitteredViewProj : prevFrame_.unjitteredViewProj;
    f.prevView = frameIndex_ == 0 ? view : prevFrame_.view;
    f.cameraPos = glm::vec4(camera_.position, camera_.nearPlane);
    f.cameraParams = glm::vec4(camera_.nearPlane, camera_.farPlane, camera_.fovY, float(renderSize_.width) / float(renderSize_.height));
    f.renderSize = glm::vec4(renderSize_.width, renderSize_.height, 1.0f / renderSize_.width, 1.0f / renderSize_.height);
    f.outputSize = glm::vec4(outputSize_.width, outputSize_.height, 1.0f / outputSize_.width, 1.0f / outputSize_.height);
    f.jitter = glm::vec4(camera_.jitterPixels, camera_.prevJitterPixels);
    f.time = glm::vec4(float(time_), ctx.dt, float(frameIndex_ % 100000), renderScale);
    f.sunDirection = glm::vec4(glm::normalize(scene_.sun.direction), scene_.sun.intensity);
    f.sunColor = glm::vec4(scene_.sun.color, scene_.sun.angularRadius);
    f.display = glm::vec4(1.0f, settings_.getFloat("display.gamma"), settings_.getFloat("display.brightness"), settings_.getFloat("display.hdr.paperWhiteNits"));
    float mipBias = renderScale < 1.0f ? std::log2(renderScale) : 0.0f;
    f.display2 = glm::vec4(settings_.getFloat("display.hdr.peakNits"), float(settings_.getInt("display.hdr.mode")), mipBias,
                           settings_.getFloat("display.hdr.blackLevelNits"));
    f.counts = glm::vec4(float(scene_.lights.size()), float(scene_.instances.size()), float(scene_.materials.size()), 0.0f);
    ctx.cmd.updateBuffer(frameUBO_.get(), f);
}

void Engine::updateDynamicResolution() {
    if (!settings_.getBool("display.dynamicRes") || upscalerRenderScale > 0.0f) {
        if (dynamicScale_ != 1.0f) { dynamicScale_ = 1.0f; requestResize = true; }
        return;
    }
    if (frameIndex_ % 15 != 0) return;
    double target = 1000.0 / std::max(1.0f, settings_.getFloat("display.dynamicRes.targetFps"));
    FrameSample a = profiler_.average(15);
    double cost = std::max(a.gpuMs, a.cpuMs);
    if (cost <= 0.0) return;
    // Pixel count scales ~ with cost: new scale = scale * sqrt(target / cost), damped.
    float ratio = float(std::sqrt(target / cost));
    float next = dynamicScale_ * std::clamp(ratio, 0.9f, 1.05f);
    float base = settings_.getFloat("display.renderScale") / 100.0f;
    float minS = settings_.getFloat("display.dynamicRes.min") / 100.0f / base;
    float maxS = settings_.getFloat("display.dynamicRes.max") / 100.0f / base;
    next = std::clamp(next, minS, maxS);
    if (std::abs(next - dynamicScale_) > 0.02f) { dynamicScale_ = next; requestResize = true; }
}

void Engine::processInput(float dt) {
    if (gui_->wantsKeyboard()) return;
    auto key = [&](int k) { return glfwGetKey(window_, k) == GLFW_PRESS; };
    static bool prev[512] = {};
    auto pressed = [&](int k) { bool p = key(k); bool r = p && !prev[k]; prev[k] = p; return r; };
    if (pressed(GLFW_KEY_F1)) gui_->showMenu = !gui_->showMenu;
    if (pressed(GLFW_KEY_F2)) gui_->showOverlay = !gui_->showOverlay;
    if (pressed(GLFW_KEY_F5)) shaders_.reloadAll();
    if (pressed(GLFW_KEY_F12)) pendingScreenshot_ = "screenshot_" + std::to_string(frameIndex_) + ".png";
    if (pressed(GLFW_KEY_ESCAPE) && !gui_->showMenu) glfwSetWindowShouldClose(window_, 1);

    bool look = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS && !gui_->wantsMouse();
    double mx, my;
    glfwGetCursorPos(window_, &mx, &my);
    if (look && mouseLooking_) {
        float sens = settings_.getFloat("input.mouseSensitivity") * 0.0025f;
        camera_.yaw += float(mx - lastMouseX_) * sens;
        camera_.pitch = std::clamp(camera_.pitch - float(my - lastMouseY_) * sens, -1.55f, 1.55f);
    }
    mouseLooking_ = look;
    lastMouseX_ = mx; lastMouseY_ = my;
    if (look) {
        float speed = (key(GLFW_KEY_LEFT_SHIFT) ? 12.0f : 3.0f) * dt;
        glm::vec3 move(0.0f);
        if (key(GLFW_KEY_W)) move += camera_.forward();
        if (key(GLFW_KEY_S)) move -= camera_.forward();
        if (key(GLFW_KEY_D)) move += camera_.right();
        if (key(GLFW_KEY_A)) move -= camera_.right();
        if (key(GLFW_KEY_E)) move += glm::vec3(0, 1, 0);
        if (key(GLFW_KEY_Q)) move -= glm::vec3(0, 1, 0);
        camera_.position += move * speed;
    }
}

void Engine::writeScreenshot(const std::string& path) {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    if (!device_->readBackbuffer(px, w, h)) return;
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    if (stbi_write_png(path.c_str(), int(w), int(h), 4, px.data(), int(w) * 4)) std::printf("[engine] screenshot %s\n", path.c_str());
}

void Engine::applyDisplaySettings() {
    int vs = settings_.getInt("display.vsync");   // Off, On, Adaptive, Half rate
    rhi::PresentMode modes[] = {rhi::PresentMode::Immediate, rhi::PresentMode::VSync, rhi::PresentMode::AdaptiveVSync, rhi::PresentMode::HalfRateVSync};
    device_->setPresentMode(modes[std::clamp(vs, 0, 3)]);
    int hdr = settings_.getInt("display.hdr.mode");
    rhi::OutputColorSpace cs[] = {rhi::OutputColorSpace::SDR_sRGB, rhi::OutputColorSpace::HDR10_PQ, rhi::OutputColorSpace::scRGB_Linear};
    hdrSwapchainActive_ = device_->setOutputColorSpace(cs[std::clamp(hdr, 0, 2)]);
}

void Engine::applyWindowMode() {
    int mode = settings_.getInt("display.windowMode");   // Windowed, Borderless, Fullscreen
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
    if (!vm) return;
    static const int res[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    int r = std::clamp(settings_.getInt("display.resolution"), 0, 4);
    if (mode == 0) {
        glfwSetWindowMonitor(window_, nullptr, 80, 80, res[r][0], res[r][1], GLFW_DONT_CARE);
        glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_TRUE);
    } else if (mode == 1) {
        glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowMonitor(window_, nullptr, 0, 0, vm->width, vm->height, GLFW_DONT_CARE);
    } else {
        glfwSetWindowMonitor(window_, mon, 0, 0, res[r][0], res[r][1], vm->refreshRate);
    }
    applyDisplaySettings();   // swap interval is per-context state that some drivers reset
}

// ---------------------------------------------------------------------------------------
void Engine::run() {
    if (!cfg_.benchSettingsDir.empty()) { runBenchmarkSweep(); return; }
    while (!glfwWindowShouldClose(window_)) {
        frame();
        if (cfg_.maxFrames > 0 && int(frameIndex_) >= cfg_.maxFrames) {
            if (!cfg_.screenshotPath.empty()) { pendingScreenshot_ = cfg_.screenshotPath; frame(); }
            break;
        }
        if (shaders_.lastError().size() && frameIndex_ == 1) std::fprintf(stderr, "[shaders] %s\n", shaders_.lastError().c_str());
    }
}

void Engine::runBenchmarkSweep() {
    auto runFrames = [&](int n) { for (int i = 0; i < n && !glfwWindowShouldClose(window_); ++i) frame(); };
    auto window = [&]() { runFrames(Profiler::kWarmup); runFrames(Profiler::kWindow); return profiler_.average(Profiler::kWindow); };
    settings_.set("display.vsync", 0);
    settings_.set("display.frameCap", 0);
    applyDisplaySettings();
    applyingPreset_ = true;
    settings_.applyPreset(Preset::High);
    applyingPreset_ = false;
    runFrames(60);
    for (auto& s : settings_.all()) {
        if (s.desc.flags & (SettingRequiresRestart | SettingDebug)) continue;
        if (s.desc.category == "Display" && (s.desc.id == "display.windowMode" || s.desc.id == "display.resolution" || s.desc.id == "display.vsync" || s.desc.id == "display.frameCap")) continue;
        if (s.desc.id == "scene.select") continue;
        std::vector<float> values;
        switch (s.type) {
            case SettingType::Bool: values = {0, 1}; break;
            case SettingType::Enum: for (int i = 0; i < int(s.options.size()); ++i) values.push_back(float(i)); break;
            case SettingType::Int: {
                int span = s.imax - s.imin;
                int step = std::max(1, span / 4);
                for (int v = s.imin; v <= s.imax; v += step) values.push_back(float(v));
                break;
            }
            case SettingType::Float: values = {s.fmin, s.defaultValue, s.fmax}; break;
        }
        float original = s.asNumber();
        FrameSample base = window();
        for (float v : values) {
            settings_.set(s.desc.id, v);
            handleResize();
            FrameSample m = window();
            Profiler::SweepResult r;
            r.settingId = s.desc.id; r.label = s.desc.label; r.category = s.desc.category; r.value = s.valueString();
            r.cost.gpuMs = m.gpuMs - base.gpuMs; r.cost.cpuMs = m.cpuMs - base.cpuMs; r.cost.frameMs = m.frameMs - base.frameMs;
            r.cost.vramMB = m.vramMB - base.vramMB; r.cost.ramMB = m.ramMB - base.ramMB; r.cost.valid = true;
            profiler_.sweepResults.push_back(r);
            std::printf("[bench] %-36s %-14s gpu %+7.3f cpu %+7.3f vram %+7.1f MB\n", s.desc.id.c_str(), r.value.c_str(), r.cost.gpuMs, r.cost.cpuMs, r.cost.vramMB);
        }
        settings_.set(s.desc.id, original);
        handleResize();
    }
    std::stable_sort(profiler_.sweepResults.begin(), profiler_.sweepResults.end(), [&](auto& a, auto& b) {
        auto cats = settings_.categories();
        auto ia = std::find(cats.begin(), cats.end(), a.category) - cats.begin();
        auto ib = std::find(cats.begin(), cats.end(), b.category) - cats.begin();
        return ia < ib;
    });
    profiler_.writeSweepReport(cfg_.benchSettingsDir, device_->caps());
    std::printf("[bench] wrote %s\n", cfg_.benchSettingsDir.c_str());
}

void Engine::shutdown() {
    if (!device_) return;
    device_->waitIdle();
    settings_.save(cfg_.settingsPath);
    for (auto& m : modules_) m->shutdown(*this);
    modules_.clear();
    gui_.reset();
    passes_ = PassList();
    targets_.reset();
    upscalePipeline_[0].reset(); upscalePipeline_[1].reset(); upscalePipeline_[2].reset();
    corePipelines_.clear();
    coreBuffers_.clear();
    renderer_ = SceneRenderer();
    scene_ = Scene();
    shaders_ = ShaderLibrary();
    frameUBO_.reset(); linearClamp_.reset(); pointClamp_.reset(); linearRepeat_.reset();
    profiler_ = Profiler();
    device_.reset();
    if (window_) glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
}

}  // namespace gfx
