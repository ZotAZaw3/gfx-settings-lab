// Camera, DrawList and SceneRenderer (the core draw loop every module reuses).
#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "core/frame.h"
#include "rhi/rhi.h"

namespace gfx {

struct Camera {
    glm::vec3 position{0.0f, 2.0f, 6.0f};
    float yaw = -1.5707963f, pitch = -0.1f;   // radians
    float fovY = 1.0472f;                     // set by the "Field of view" setting
    float nearPlane = 0.05f;
    float farPlane = 500.0f;                  // draw distance (culling); projection is infinite reversed-Z
    glm::vec2 jitterPixels{0.0f};             // set in Stage::FrameSetup by AA/upscaler (Halton etc.)
    glm::vec2 prevJitterPixels{0.0f};

    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const;
    glm::mat4 viewMatrix() const;
    // Infinite reversed-Z, D3D clip space. jitter in pixels of `size`.
    glm::mat4 projection(Extent size, glm::vec2 jitter = glm::vec2(0.0f)) const;
    void setJitter(glm::vec2 pixels) { jitterPixels = pixels; }
};

struct DrawItem {
    uint32_t instance = 0;
    uint32_t primitive = 0;
    uint32_t lod = 0;            // index into Primitive::lods
    float    distance = 0.0f;    // camera distance (for sorting)
};

struct DrawList {
    std::vector<DrawItem> opaque;        // Opaque + Mask
    std::vector<DrawItem> transparent;   // Blend, sorted back to front
    uint32_t culled = 0;                 // filled by the culling module for stats
    void clear() { opaque.clear(); transparent.clear(); culled = 0; }
};

// Per-draw constants, std140, UBO binding 1. Mirrored in shaders/common/draw.glsl.
struct DrawConstants {
    glm::mat4 world;
    glm::mat4 prevWorld;
    glm::mat4 viewProj;           // the pass' view-projection (camera or shadow cascade)
    glm::mat4 prevViewProj;       // for velocity (camera passes)
    glm::uvec4 ids;               // x material index, y instance, z primitive, w lod
    glm::vec4 params;             // x mip bias, y alpha-test (1/0), zw unused
};

enum class DrawMode {
    DepthOnly,   // shadow maps / prepass: position only, alpha-tested materials still clip
    GBuffer,     // core deferred: writes gbuffer.* + velocity
    Forward,     // core forward (MSAA): shades via lighting/shade.glsl
    Custom,      // your program; core binds vertex buffers, DrawConstants, material textures 0-5
};

struct SceneDrawParams {
    DrawMode       mode = DrawMode::GBuffer;
    glm::mat4      viewProj{1.0f};
    glm::mat4      prevViewProj{1.0f};
    rhi::Pipeline* pipeline = nullptr;      // required for Custom; optional override otherwise
    rhi::Pipeline* maskedPipeline = nullptr;// optional separate pipeline for alpha-masked materials
    const DrawList* list = nullptr;         // nullptr = ctx.draws
    bool           transparent = false;     // draw list->transparent instead of opaque
    bool           shadowCastersOnly = false;
    float          mipBias = 0.0f;
};

class SceneRenderer {
public:
    void init(class Engine& engine);
    // Binds: vertex/index buffers, materials SSBO(1), lights SSBO(0), DrawConstants UBO(1),
    // material textures at slots 0..5 (baseColor, normal, metalRough, occlusion, emissive, height).
    void drawScene(FrameContext& ctx, const SceneDrawParams& params);
    void buildDefaultDrawList(FrameContext& ctx);   // every instance/primitive at LOD0
    void beginFrame();                               // resets per-frame counters + constants ring
    rhi::Sampler* materialSampler() const { return materialSampler_.get(); }
    void setMaterialSampler(rhi::SamplerPtr s) { materialSampler_ = std::move(s); }   // texture filtering setting
    rhi::Buffer* drawUBO() const { return drawUBO_.get(); }
    static rhi::VertexLayout vertexLayout();

    uint32_t drawCallsThisFrame = 0, trianglesThisFrame = 0;

private:
    class Engine* engine_ = nullptr;
    rhi::BufferPtr drawUBO_;
    rhi::SamplerPtr materialSampler_;
    std::vector<uint8_t> staging_;
    uint32_t ringHead_ = 0;
    rhi::PipelinePtr pipelines_[3][2][2];       // [DepthOnly/GBuffer/Forward][alphaTest][twoSided]
    rhi::PipelinePtr transparentPipeline_[2];
};

}  // namespace gfx
