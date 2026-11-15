// Scene data: CPU copies (for BVH builds, LOD generation, culling) + GPU buffers.
// All primitives share ONE vertex buffer and ONE index buffer (vertex pulling / RT friendly).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "rhi/rhi.h"

namespace gfx {

struct Vertex {                 // 48 bytes, matches shaders/common/draw.glsl locations 0..3
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;          // xyz tangent, w handedness
    glm::vec2 uv;
};
static_assert(sizeof(Vertex) == 48);

struct AABB {
    glm::vec3 min{1e30f}, max{-1e30f};
    void expand(const glm::vec3& p) { min = glm::min(min, p); max = glm::max(max, p); }
    void expand(const AABB& b) { expand(b.min); expand(b.max); }
    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extent() const { return (max - min) * 0.5f; }
    bool valid() const { return min.x <= max.x; }
    AABB transformed(const glm::mat4& m) const;
};

enum class AlphaMode : int { Opaque = 0, Mask = 1, Blend = 2 };
enum class ShadingModel : int { Default = 0, Subsurface = 1, Foliage = 2, Unlit = 3, ClearCoat = 4 };

// Texture owned by the scene. The streaming system (core_features) decides which mips are resident.
struct SceneTexture {
    std::string name;
    bool srgb = true;
    uint32_t width = 0, height = 0, mipCount = 0;
    std::vector<std::vector<uint8_t>> cpuMips;   // RGBA8 mip chain kept in RAM (streaming source)
    rhi::TexturePtr gpu;                          // current GPU texture (may hold fewer mips)
    uint32_t residentTopMip = 0;                  // highest-res mip currently on GPU (0 = full)
    uint32_t wantedTopMip = 0;                    // streaming target
    float    lastUsedFrame = 0;                   // for LRU eviction
    float    minScreenTexelRatio = 1e9f;          // feedback: smallest texel/pixel footprint seen
};

struct Material {
    std::string  name;
    glm::vec4    baseColor{1.0f};
    glm::vec3    emissive{0.0f};
    float        roughness = 1.0f;
    float        metallic = 0.0f;
    float        normalScale = 1.0f;
    float        occlusionStrength = 1.0f;
    float        alphaCutoff = 0.5f;
    AlphaMode    alphaMode = AlphaMode::Opaque;
    ShadingModel shadingModel = ShadingModel::Default;
    bool         doubleSided = false;
    glm::vec3    subsurfaceColor{1.0f, 0.4f, 0.25f};
    float        subsurfaceRadius = 0.0f;   // mm-ish; 0 = off
    float        displacementScale = 0.0f;  // tessellation / POM height scale
    int baseColorTex = -1, normalTex = -1, metalRoughTex = -1, occlusionTex = -1, emissiveTex = -1, heightTex = -1;
};

// GPU material record, std430, SSBO binding 1. Mirrored in shaders/common/draw.glsl.
struct GpuMaterial {
    glm::vec4 baseColor;
    glm::vec4 emissiveRough;       // rgb emissive, a roughness
    glm::vec4 params;              // x metallic, y normalScale, z occlusionStrength, w alphaCutoff
    glm::vec4 sss;                 // rgb subsurface color, a radius
    glm::ivec4 tex0;               // baseColor, normal, metalRough, occlusion (index or -1)
    glm::ivec4 tex1;               // emissive, height, alphaMode, shadingModel
    glm::vec4 params2;             // x displacementScale, y doubleSided, zw unused
};

struct LodLevel {
    uint32_t firstIndex = 0;       // into Scene::indices
    uint32_t indexCount = 0;
    float    error = 0.0f;         // simplification error (object space), 0 for LOD0
};

struct Primitive {
    uint32_t baseVertex = 0, vertexCount = 0;
    std::vector<LodLevel> lods;    // lods[0] = full detail (core); geometry module appends more
    int      material = 0;
    AABB     bounds;               // object space
};

struct Mesh {
    std::string name;
    std::vector<uint32_t> primitives;   // indices into Scene::primitives
};

struct Instance {
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
    uint32_t  mesh = 0;
    AABB      worldBounds;
    bool      castShadows = true;
};

enum class LightType : int { Point = 0, Spot = 1 };
struct Light {
    LightType type = LightType::Point;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity = 10.0f;
    float range = 10.0f;
    float innerCone = 0.3f, outerCone = 0.5f;   // radians
    bool  castShadows = false;
};
// GPU light record, std430, SSBO binding 0.
struct GpuLight {
    glm::vec4 positionRange;    // xyz, w range
    glm::vec4 colorIntensity;   // rgb, w intensity
    glm::vec4 directionType;    // xyz dir, w type
    glm::vec4 cone;             // x cos(inner), y cos(outer), z castShadows, w shadow index
};

struct Sun {
    glm::vec3 direction = glm::normalize(glm::vec3(0.3f, 0.9f, 0.25f));   // towards the sun
    glm::vec3 color{1.0f, 0.96f, 0.9f};
    float intensity = 6.0f;
    float angularRadius = 0.00465f;
};

class Scene {
public:
    // CPU data (read freely; call markGeometryDirty() after editing vertices/indices)
    std::vector<Vertex>       vertices;
    std::vector<uint32_t>     indices;
    std::vector<Primitive>    primitives;
    std::vector<Mesh>         meshes;
    std::vector<Instance>     instances;
    std::vector<Material>     materials;
    std::vector<SceneTexture> textures;
    std::vector<Light>        lights;
    Sun                       sun;
    AABB                      bounds;
    std::string               sourcePath;

    // GPU data
    rhi::BufferPtr vertexBuffer;     // also bindable as SSBO (binding 2) for vertex pulling / RT
    rhi::BufferPtr indexBuffer;      // also bindable as SSBO (binding 3)
    rhi::BufferPtr materialBuffer;   // SSBO binding 1 (GpuMaterial[])
    rhi::BufferPtr lightBuffer;      // SSBO binding 0 (GpuLight[])

    // Bumps whenever geometry/topology changes (rebuild BVHs, LODs...).
    uint64_t geometryVersion() const { return geometryVersion_; }
    uint64_t materialVersion() const { return materialVersion_; }
    void markGeometryDirty() { ++geometryVersion_; geometryDirty_ = true; }
    void markMaterialsDirty() { ++materialVersion_; materialsDirty_ = true; }

    // Appends a LOD index range for a primitive (indices are relative to the primitive's baseVertex).
    uint32_t addLod(uint32_t primitive, const std::vector<uint32_t>& lodIndices, float error);

    bool loadGltf(const std::string& path, rhi::Device& device, std::string* error = nullptr);
    void buildProceduralDemo(rhi::Device& device);   // fallback scene: spheres, cubes, plane, foliage cards
    void upload(rhi::Device& device);                // (re)creates GPU buffers if dirty
    void updateInstances();                          // prevWorld = world, recompute bounds
    size_t cpuBytes() const;

    // Fallback 1x1 textures for materials without maps (bound by core when index == -1).
    rhi::TexturePtr whiteTex, flatNormalTex, blackTex;

private:
    uint64_t geometryVersion_ = 1, materialVersion_ = 1;
    bool geometryDirty_ = true, materialsDirty_ = true;
};

}  // namespace gfx
