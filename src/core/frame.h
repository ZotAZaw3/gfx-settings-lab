// Per-frame data shared by every module: FrameConstants (UBO binding 0), the pass list with its
// fixed stages, named render targets, the post-processing ping-pong chain and FrameContext.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "rhi/rhi.h"

namespace gfx {

class Engine;
class SettingsRegistry;
class Scene;
class SceneRenderer;
class ShaderLibrary;
class Profiler;
struct Camera;
struct DrawList;

struct Extent {
    uint32_t width = 0, height = 0;
    bool operator==(const Extent& o) const { return width == o.width && height == o.height; }
    bool operator!=(const Extent& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------------------
// FrameConstants — std140, mirrored in shaders/common/frame.glsl. UBO binding 0, every pass.
// ---------------------------------------------------------------------------------------
struct FrameConstants {
    glm::mat4 view;
    glm::mat4 proj;                  // JITTERED projection (what the scene is rasterized with)
    glm::mat4 viewProj;              // view * jittered proj
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::mat4 invViewProj;
    glm::mat4 unjitteredProj;
    glm::mat4 unjitteredViewProj;    // use for velocity / reprojection
    glm::mat4 prevViewProj;          // previous frame, unjittered
    glm::mat4 prevView;
    glm::vec4 cameraPos;             // xyz world, w = near plane
    glm::vec4 cameraParams;          // x near, y far (draw distance; projection is infinite), z fovY rad, w aspect
    glm::vec4 renderSize;            // xy pixels, zw 1/size  (internal resolution)
    glm::vec4 outputSize;            // xy pixels, zw 1/size  (display resolution)
    glm::vec4 jitter;                // xy current jitter (pixels), zw previous jitter (pixels)
    glm::vec4 time;                  // x seconds, y delta seconds, z frame index, w render scale
    glm::vec4 sunDirection;          // xyz direction TOWARDS the sun (world), w intensity (lux-ish)
    glm::vec4 sunColor;              // rgb linear color, w angular radius (radians)
    glm::vec4 display;               // x exposure base (always 1; post owns exposure), y gamma, z brightness, w paper white nits
    glm::vec4 display2;              // x peak nits, y output mode (0 SDR, 1 HDR10 PQ, 2 scRGB), z mip bias, w black level nits
    glm::vec4 counts;                // x light count, y instance count, z material count, w unused
};
static_assert(sizeof(FrameConstants) % 16 == 0, "std140 alignment");

// ---------------------------------------------------------------------------------------
// Stages. Passes run in stage order, then by `order` within a stage.
// Render-res stages run at renderSize; from Upscale on, everything is at outputSize.
// ---------------------------------------------------------------------------------------
enum class Stage : int {
    FrameSetup = 0,  // camera update, jitter, culling + LOD selection (fill DrawList), BVH updates, streaming
    Shadow,          // shadow maps (lighting)
    DepthPrepass,    // optional (core, when "Depth prepass" is on)
    GBuffer,         // core deferred geometry pass (+ geometry module tessellation/POM/decals)
    Lighting,        // AO, SSR, GI, deferred resolve -> scene.color, SSS, RT effects
    Forward,         // core forward pass when MSAA is on (shading via lighting/shade.glsl)
    Translucent,     // particles, volumetric fog/clouds, transparent meshes
    PostRender,      // render-res post: DOF, motion blur (needs depth/velocity at render res)
    Upscale,         // TAA / temporal upscaler / spatial upscale: render res -> output res
    PostOutput,      // output-res HDR post: bloom, god rays, lens flare, auto-exposure
    ToneMap,         // HDR -> display-referred (post module)
    PostLDR,         // vignette, grain, chromatic aberration, grading, CAS sharpening, FXAA-LDR
    OutputTransform, // core: brightness/gamma/HDR10 PQ encoding into final.output
    FrameGen,        // frame generation (reads final.output history, may queue an interpolated frame)
    UI,              // core: ImGui + frame overlay (not seen by frame gen)
    Count
};
const char* stageName(Stage s);

struct FrameContext;
using PassFn = std::function<void(FrameContext&)>;
using EnabledFn = std::function<bool()>;

struct Pass {
    std::string name;     // shown in the GPU/CPU timing overlay, e.g. "SSAO"
    std::string owner;    // module name (filled automatically)
    Stage       stage = Stage::Lighting;
    float       order = 0.0f;
    EnabledFn   enabled;  // nullptr = always on
    PassFn      execute;
    // Measured each frame by the profiler (smoothed):
    double gpuMs = 0.0, cpuMs = 0.0;
    bool   ranThisFrame = false;
};

class PassList {
public:
    Pass& add(Stage stage, std::string name, PassFn fn, float order = 0.0f, EnabledFn enabled = nullptr);
    std::vector<Pass>& passes() { return passes_; }
    void sort();
    std::string currentOwner;   // set by Engine while calling Module::addPasses
private:
    std::vector<Pass> passes_;
};

// ---------------------------------------------------------------------------------------
// Named render targets. Core targets (always present):
//   scene.color      RGBA16F  render res   HDR lit color (lighting writes, post reads)
//   scene.depth      D32F     render res   reversed-Z, sampleable
//   scene.velocity   RG16F    render res   NDC delta (current - previous), unjittered; 0 for static sky
//   gbuffer.albedo   RGBA8_SRGB           rgb base color, a = specular occlusion / spare
//   gbuffer.normal   RGBA16F              xyz world normal (normalized), w = shading model id / 255
//   gbuffer.material RGBA8                r roughness, g metallic, b material AO, a reflectance/spare
//   gbuffer.emissive RGBA16F              rgb emissive radiance, a = SSS mask/thickness
//   final.output     RGBA16F  output res  display-encoded image after OutputTransform
// Modules can create their own (auto-resized) and share them by name, e.g. "ao.result".
// ---------------------------------------------------------------------------------------
enum class TargetScale { Render, Output, Fixed };

class RenderTargets {
public:
    explicit RenderTargets(rhi::Device& dev) : device_(dev) {}
    // Creates (or returns existing) target. For Render/Output scale, desc.width/height are
    // overwritten and the target is recreated on resize; `scale` multiplies the size (0.5 = half res).
    rhi::Texture* create(const std::string& name, rhi::TextureDesc desc, TargetScale ts = TargetScale::Render, float scale = 1.0f);
    rhi::Texture* get(const std::string& name) const;   // nullptr if missing
    void remove(const std::string& name);
    void resize(Extent render, Extent output);
    const std::map<std::string, rhi::TexturePtr>& all() const { return textures_; }
private:
    struct Entry { rhi::TextureDesc desc; TargetScale ts; float scale; };
    rhi::Device& device_;
    std::map<std::string, rhi::TexturePtr> textures_;
    std::map<std::string, Entry> entries_;
    Extent render_, output_;
};

// Ping-pong HDR chain for post-processing (RGBA16F).
//   src()  : current image (starts as scene.color)
//   dst()  : scratch target at the current stage's resolution (render res before Upscale,
//            output res from Upscale on). Write it, then call swap().
// If no Upscale pass calls swap(), core upsamples src() to output res itself (render scale).
class PostChain {
public:
    rhi::Texture* src() const { return src_; }
    rhi::Texture* dst();
    void swap();
    bool upscaled() const { return atOutputRes_; }
    // internal (Engine)
    void beginFrame(rhi::Texture* sceneColor, rhi::Texture* renderA, rhi::Texture* outA, rhi::Texture* outB);
    void enterOutputRes() { inUpscale_ = true; }
    void forceSrc(rhi::Texture* t, bool outputRes) { src_ = t; atOutputRes_ = outputRes; }
    rhi::Texture* sceneColor() const { return sceneColor_; }
private:
    rhi::Texture* sceneColor_ = nullptr;
    rhi::Texture* src_ = nullptr;
    rhi::Texture* renderScratch_ = nullptr;
    rhi::Texture* outA_ = nullptr;
    rhi::Texture* outB_ = nullptr;
    rhi::Texture* pendingDst_ = nullptr;
    bool inUpscale_ = false, atOutputRes_ = false;
};

// ---------------------------------------------------------------------------------------
// FrameContext — passed to every pass.
// ---------------------------------------------------------------------------------------
struct FrameContext {
    Engine&           engine;
    rhi::Device&      device;
    rhi::CommandList& cmd;
    SettingsRegistry& settings;
    Scene&            scene;
    SceneRenderer&    renderer;
    ShaderLibrary&    shaders;
    RenderTargets&    targets;
    PostChain&        post;
    Profiler&         profiler;
    Camera&           camera;
    DrawList&         draws;            // visible draw items this frame (geometry module filters it)

    FrameConstants    frame{};          // CPU copy of the UBO
    rhi::Buffer*      frameUBO = nullptr;   // already bound at binding 0 for every pass
    Extent            renderSize, outputSize;
    uint64_t          frameIndex = 0;
    float             dt = 0.0f, time = 0.0f;
    bool              msaaForward = false;  // true -> Forward stage draws the scene instead of GBuffer+Lighting
    // Path tracing sets this in Stage::FrameSetup: raster lighting passes (resolve, SSAO, SSR, shadows...)
    // must early-out when true; the path tracer writes scene.color itself in Stage::Lighting.
    bool              replaceLighting = false;

    // Frame generation: present this image (output res, display encoded) BEFORE the real frame.
    rhi::Texture* interpolatedFrame = nullptr;

    // Convenience: run a fullscreen triangle with `pipeline` into `target` (binds frame UBO).
    void fullscreen(rhi::Pipeline* pipeline, rhi::Texture* target, const char* name = "fullscreen");
};

}  // namespace gfx
