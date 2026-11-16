// Shared helpers for the geometry modules (src/modules/geometry/*):
// procedural meshes, a vertex-clustering simplifier for LOD chains, frustum math,
// the showcase terrain heightfield (CPU copy), and G-buffer render-pass helpers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "core/engine.h"

namespace geo {

// Showcase area (MODULE_API: core demo sits within ~25 m of the origin, so we use x 20..180, z -180..-20)
inline constexpr float kTerrainSize = 160.0f;
inline const glm::vec3 kTerrainCenter{100.0f, 0.0f, -100.0f};
inline const glm::vec3 kPlaza{42.0f, 0.0f, -42.0f};     // flat area: particles, POM floor, decals, fur, cloth, crowd
inline constexpr float kPlazaRadius = 20.0f;
inline const glm::vec3 kLake{110.0f, 0.0f, -110.0f};
inline constexpr float kLakeRadius = 28.0f;
inline constexpr float kWaterLevel = 1.0f;

// ---- simple CPU mesh -------------------------------------------------------------------
struct SimpleVertex { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv; };   // 32 bytes
struct SimpleMesh {
    std::vector<SimpleVertex> vertices;
    std::vector<uint32_t> indices;
};
SimpleMesh makeSphere(int segments, int rings, float radius = 1.0f);
SimpleMesh makeTorusKnot(int segments, int sides, float radius = 1.0f, float tube = 0.3f, int p = 2, int q = 3);
SimpleMesh makeCapsule(int segments, float radius, float height);
SimpleMesh makeBox();   // unit cube [-0.5, 0.5]^3
SimpleMesh makeGridXZ(int n, float size);   // n x n quads, centered, y = 0
void computeNormals(SimpleMesh& m);

// Vertex-clustering simplifier: snaps vertices to a grid with `cells` cells across the
// bounding box diagonal axis, merges per cell, drops degenerate triangles. Returns new
// index list referencing the ORIGINAL vertex array (representative vertex per cell), so it
// can be used directly as a LOD index range (Scene::addLod). `outError` = cell size.
std::vector<uint32_t> simplifyClustered(const glm::vec3* positions, size_t vertexCount, size_t stride,
                                        const std::vector<uint32_t>& indices, int cells, float* outError);

// ---- math --------------------------------------------------------------------------------
struct Frustum { glm::vec4 planes[5]; };   // left, right, bottom, top, near (reversed-Z infinite)
Frustum extractFrustum(const glm::mat4& viewProj);
bool sphereInFrustum(const Frustum& f, const glm::vec3& c, float r);
bool aabbInFrustum(const Frustum& f, const glm::vec3& mn, const glm::vec3& mx);
// pixels per world unit at distance 1 (proj[1][1] * height / 2)
float projScaleY(const gfx::FrameConstants& fc);

// ---- terrain heightfield (CPU mirror of the GPU texture) ---------------------------------
struct HeightField {
    int res = 0;
    std::vector<float> h;            // normalized 0..1 (multiplied by displacement scale in shaders)
    float sample(float worldX, float worldZ) const;   // normalized, bilinear
    void generate(int resolution);
};
const HeightField& terrainHeights();
float terrainHeightWorld(float x, float z, float dispScale);

// ---- GPU helpers ---------------------------------------------------------------------------
gfx::rhi::BufferPtr makeBuffer(gfx::Engine& e, size_t bytes, uint32_t usage, const char* name,
                               const char* owner, const void* data = nullptr, uint32_t stride = 0);
gfx::rhi::VertexLayout simpleVertexLayout();   // SimpleVertex at locations 0 pos, 1 normal, 3 uv

// Begins a render pass on the core G-buffer (load everything). colorMaskForDecals: only
// albedo/normal/material attachments are bound when true.
void beginGBufferPass(gfx::FrameContext& ctx, const char* name);
gfx::rhi::PipelineDesc gbufferPipelineDesc(gfx::rhi::Program* prog, const char* name);

// Settings helper: id prefix "geometry." etc.
inline const char* kOwnerLod = "geometry.lod";

}  // namespace geo
