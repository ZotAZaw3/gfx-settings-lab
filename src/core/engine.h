// Engine — owns the window, device, modules, settings, scene and runs the frame loop.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/frame.h"
#include "core/module.h"
#include "core/profiler.h"
#include "core/renderer.h"
#include "core/scene.h"
#include "core/settings.h"
#include "core/shader_library.h"
#include "rhi/rhi.h"

struct GLFWwindow;

namespace gfx {

class CoreGui;

struct EngineConfig {
    rhi::Backend backend = rhi::Backend::OpenGL;
    uint32_t width = 1600, height = 900;
    std::string scenePath;            // glTF; empty = procedural demo scene
    std::string settingsPath = "settings.ini";
    std::string shaderRoot;           // default: <exe dir>/shaders or ./shaders
    int  maxFrames = 0;               // >0: exit after N frames (CI / screenshots)
    std::string screenshotPath;       // written at the last frame when maxFrames > 0
    std::string benchSettingsDir;     // non-empty: run the per-setting benchmark sweep, write report, exit
    bool hidden = false;              // headless (Xvfb) runs
};

class Engine {
public:
    Engine();
    ~Engine();
    bool init(const EngineConfig& cfg);
    void run();
    void shutdown();

    rhi::Device&      device()   { return *device_; }
    SettingsRegistry& settings() { return settings_; }
    ShaderLibrary&    shaders()  { return shaders_; }
    Scene&            scene()    { return scene_; }
    SceneRenderer&    renderer() { return renderer_; }
    RenderTargets&    targets()  { return *targets_; }
    Profiler&         profiler() { return profiler_; }
    Camera&           camera()   { return camera_; }
    GLFWwindow*       window()   { return window_; }
    const EngineConfig& config() const { return cfg_; }

    Extent renderSize() const { return renderSize_; }
    Extent outputSize() const { return outputSize_; }
    bool   windowFocused() const { return focused_; }
    uint64_t frameIndex() const { return frameIndex_; }
    Module* findModule(const std::string& name);
    const std::vector<std::unique_ptr<Module>>& modules() const { return modules_; }

    // Common pipelines
    rhi::Program* fullscreenProgram(const std::string& fs, std::vector<std::string> defines = {});
    rhi::PipelinePtr createFullscreenPipeline(const std::string& fs, std::vector<std::string> defines = {},
                                              rhi::BlendMode blend = rhi::BlendMode::Opaque);
    rhi::Sampler* linearClamp()  { return linearClamp_.get(); }
    rhi::Sampler* pointClamp()   { return pointClamp_.get(); }
    rhi::Sampler* linearRepeat() { return linearRepeat_.get(); }

    // Core-feature hooks (set by modules/core_features)
    float renderScale = 1.0f;          // effective render scale this frame (read-only for modules)
    float upscalerRenderScale = 0.0f;  // AA module: >0 while a temporal upscaler owns the render scale
    bool  requestResize = false;
    void  requestScreenshot(const std::string& path) { pendingScreenshot_ = path; }
    void  applyDisplaySettings();      // vsync / present mode / HDR swapchain
    void  applyWindowMode();           // window mode + resolution
    bool  hdrSwapchainActive() const { return hdrSwapchainActive_; }
    int   msaaSamples() const { return msaaSamples_; }
    uint64_t presentedFrames() const { return presentedFrames_; }
    const std::vector<std::string>& sceneFiles() const { return sceneFiles_; }
    PassList& passList() { return passes_; }
    CoreGui* gui() { return gui_.get(); }
    FrameContext* currentFrame() { return curCtx_; }

private:
    void frame();
    void handleResize();
    void createCoreTargets();
    void runStage(FrameContext& ctx, Stage s);
    void setupCoreSettings();
    void addCorePasses();
    void runBenchmarkSweep();
    void writeScreenshot(const std::string& path);
    void updateRenderSize();
    void fillFrameConstants(FrameContext& ctx);
    void updateDynamicResolution();
    void processInput(float dt);

    EngineConfig cfg_;
    GLFWwindow* window_ = nullptr;
    std::unique_ptr<rhi::Device> device_;
    SettingsRegistry settings_;
    ShaderLibrary shaders_;
    Scene scene_;
    SceneRenderer renderer_;
    std::unique_ptr<RenderTargets> targets_;
    PostChain post_;
    Profiler profiler_;
    Camera camera_;
    DrawList draws_;
    PassList passes_;
    std::vector<std::unique_ptr<Module>> modules_;
    rhi::BufferPtr frameUBO_;
    rhi::SamplerPtr linearClamp_, pointClamp_, linearRepeat_;
    rhi::PipelinePtr upscalePipeline_[3];

    Extent renderSize_, outputSize_;
    FrameConstants prevFrame_{};
    uint64_t frameIndex_ = 0;
    double time_ = 0.0, lastTime_ = 0.0;
    bool focused_ = true;
    std::string pendingScreenshot_;
    bool quit_ = false;
    bool applyingPreset_ = false;
    bool hasLightingModule_ = false, hasToneMapModule_ = false;
    bool hdrSwapchainActive_ = false;
    int  msaaSamples_ = 1;
    float dynamicScale_ = 1.0f;
    double lastFrameStart_ = 0.0;
    uint64_t presentedFrames_ = 0;
    bool mouseLooking_ = false;
    double lastMouseX_ = 0, lastMouseY_ = 0;
    std::vector<std::string> sceneFiles_;
    std::vector<rhi::PipelinePtr> corePipelines_;
    std::vector<rhi::BufferPtr> coreBuffers_;
    int pendingSceneLoad_ = -1;
    std::unique_ptr<CoreGui> gui_;
    FrameContext* curCtx_ = nullptr;
    friend class CoreGui;
};

}  // namespace gfx
