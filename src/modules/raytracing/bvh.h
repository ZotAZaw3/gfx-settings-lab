// CPU-built binned-SAH BVH, flattened for GPU traversal.
// See docs/raytracing_overview.md ("BVH build").
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace gfx::rt {

// GPU layout (std430, 32 bytes). Mirrors `BVHNode` in shaders/raytracing/rt_common.glsl.
// Interior node: count == 0, children are nodes [leftOrFirst] and [leftOrFirst + 1].
// Leaf: `count` triangles starting at triangle index leftOrFirst.
struct BVHNode {
    glm::vec3 bmin; uint32_t leftOrFirst;
    glm::vec3 bmax; uint32_t count;
};
static_assert(sizeof(BVHNode) == 32, "BVHNode must be 32 bytes");

// World-space triangle positions (std430: 3 x vec4 = 48 bytes). w unused (0).
struct GpuTri {
    glm::vec4 v0, v1, v2;
};
// Per-triangle shading data. n0.w holds the material index as a float.
struct GpuTriAttr {
    glm::vec4 n0, n1, n2;
};
// Mirrors `Material` in rt_common.glsl.
struct GpuMaterial {
    glm::vec4 albedoRoughness;   // rgb base color factor, a roughness
    glm::vec4 emissiveMetallic;  // rgb emissive (linear, pre-multiplied by strength), a metallic
    glm::vec4 transmissionIor;   // x transmission (0..1), y ior, zw unused
};
static_assert(sizeof(GpuMaterial) == 48, "GpuMaterial must be 48 bytes");

struct BuildTriangle {
    glm::vec3 p[3];
    glm::vec3 n[3];
    uint32_t material;
};

struct BVHStats {
    uint32_t nodes = 0, leaves = 0, maxDepth = 0, triangles = 0;
    double buildMs = 0.0;
};

class BVH {
public:
    // Builds the tree and reorders triangles so each leaf references a contiguous range.
    void build(const std::vector<BuildTriangle>& tris, uint32_t maxLeafTris = 4);

    const std::vector<BVHNode>& nodes() const { return nodes_; }
    const std::vector<GpuTri>& gpuTris() const { return tris_; }
    const std::vector<GpuTriAttr>& gpuAttrs() const { return attrs_; }
    const BVHStats& stats() const { return stats_; }

    // CPU reference traversal, used by the self-test. Returns hit distance or -1.
    float intersect(const glm::vec3& o, const glm::vec3& d, float tMax, uint32_t* triOut = nullptr) const;

private:
    struct Bounds {
        glm::vec3 mn{ 1e30f }, mx{ -1e30f };
        void grow(const glm::vec3& p) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
        void grow(const Bounds& b) { mn = glm::min(mn, b.mn); mx = glm::max(mx, b.mx); }
        float area() const {
            glm::vec3 e = mx - mn;
            if (e.x < 0.f) return 0.f;
            return 2.f * (e.x * e.y + e.y * e.z + e.z * e.x);
        }
    };
    void subdivide(uint32_t nodeIdx, uint32_t depth);
    void updateBounds(uint32_t nodeIdx);
    float findBestSplit(const BVHNode& node, int& axis, float& splitPos) const;

    std::vector<BVHNode> nodes_;
    std::vector<GpuTri> tris_;
    std::vector<GpuTriAttr> attrs_;
    std::vector<glm::vec3> centroids_;
    std::vector<uint32_t> order_;
    uint32_t used_ = 0;
    uint32_t maxLeaf_ = 4;
    BVHStats stats_;
};

} // namespace gfx::rt
