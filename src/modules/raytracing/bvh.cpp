#include "bvh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace gfx::rt {

namespace {
constexpr int kBins = 12;
constexpr float kTraversalCost = 1.0f;
}

void BVH::build(const std::vector<BuildTriangle>& in, uint32_t maxLeafTris) {
    auto t0 = std::chrono::high_resolution_clock::now();
    maxLeaf_ = std::max(1u, maxLeafTris);
    const uint32_t n = static_cast<uint32_t>(in.size());

    tris_.clear(); attrs_.clear(); nodes_.clear();
    centroids_.resize(n);
    order_.resize(n);
    std::vector<GpuTri> srcTris(n);
    std::vector<GpuTriAttr> srcAttrs(n);
    for (uint32_t i = 0; i < n; ++i) {
        const auto& t = in[i];
        srcTris[i] = { glm::vec4(t.p[0], 0.f), glm::vec4(t.p[1], 0.f), glm::vec4(t.p[2], 0.f) };
        srcAttrs[i] = { glm::vec4(t.n[0], float(t.material)), glm::vec4(t.n[1], 0.f), glm::vec4(t.n[2], 0.f) };
        centroids_[i] = (t.p[0] + t.p[1] + t.p[2]) * (1.f / 3.f);
        order_[i] = i;
    }
    tris_ = srcTris; // working copy, permuted below via order_

    stats_ = {};
    stats_.triangles = n;
    if (n == 0) {
        // One empty leaf with inverted bounds so traversal misses immediately.
        nodes_.push_back({ glm::vec3(1e30f), 0u, glm::vec3(-1e30f), 0u });
        nodes_[0].count = 0; nodes_[0].leftOrFirst = 0;
        stats_.nodes = 1;
        return;
    }

    nodes_.resize(size_t(n) * 2);
    used_ = 1;
    BVHNode& root = nodes_[0];
    root.leftOrFirst = 0;
    root.count = n;
    updateBounds(0);
    subdivide(0, 1);
    nodes_.resize(used_);

    // Apply the final permutation so leaves index contiguous triangles.
    tris_.resize(n); attrs_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        tris_[i] = srcTris[order_[i]];
        attrs_[i] = srcAttrs[order_[i]];
    }
    centroids_.clear(); centroids_.shrink_to_fit();

    stats_.nodes = used_;
    for (const auto& nd : nodes_) if (nd.count) stats_.leaves++;
    stats_.buildMs = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
}

void BVH::updateBounds(uint32_t idx) {
    BVHNode& node = nodes_[idx];
    Bounds b;
    for (uint32_t i = 0; i < node.count; ++i) {
        const GpuTri& t = tris_[order_[node.leftOrFirst + i]];
        b.grow(glm::vec3(t.v0)); b.grow(glm::vec3(t.v1)); b.grow(glm::vec3(t.v2));
    }
    node.bmin = b.mn; node.bmax = b.mx;
}

float BVH::findBestSplit(const BVHNode& node, int& bestAxis, float& bestPos) const {
    float bestCost = 1e30f;
    for (int axis = 0; axis < 3; ++axis) {
        float cmin = 1e30f, cmax = -1e30f;
        for (uint32_t i = 0; i < node.count; ++i) {
            float c = centroids_[order_[node.leftOrFirst + i]][axis];
            cmin = std::min(cmin, c); cmax = std::max(cmax, c);
        }
        if (cmax - cmin < 1e-6f) continue;

        Bounds binB[kBins];
        uint32_t binN[kBins] = {};
        float scale = kBins / (cmax - cmin);
        for (uint32_t i = 0; i < node.count; ++i) {
            uint32_t ti = order_[node.leftOrFirst + i];
            int b = std::min(kBins - 1, int((centroids_[ti][axis] - cmin) * scale));
            binN[b]++;
            const GpuTri& t = tris_[ti];
            binB[b].grow(glm::vec3(t.v0)); binB[b].grow(glm::vec3(t.v1)); binB[b].grow(glm::vec3(t.v2));
        }
        // Sweep left->right and right->left to get SAH cost for each of the 11 planes.
        float leftArea[kBins - 1], rightArea[kBins - 1];
        uint32_t leftN[kBins - 1], rightN[kBins - 1];
        Bounds lb, rb; uint32_t ls = 0, rs = 0;
        for (int i = 0; i < kBins - 1; ++i) {
            ls += binN[i]; leftN[i] = ls; lb.grow(binB[i]); leftArea[i] = lb.area();
            rs += binN[kBins - 1 - i]; rightN[kBins - 2 - i] = rs;
            rb.grow(binB[kBins - 1 - i]); rightArea[kBins - 2 - i] = rb.area();
        }
        float step = (cmax - cmin) / kBins;
        for (int i = 0; i < kBins - 1; ++i) {
            if (leftN[i] == 0 || rightN[i] == 0) continue;
            float cost = leftN[i] * leftArea[i] + rightN[i] * rightArea[i];
            if (cost < bestCost) { bestCost = cost; bestAxis = axis; bestPos = cmin + step * (i + 1); }
        }
    }
    return bestCost;
}

void BVH::subdivide(uint32_t idx, uint32_t depth) {
    stats_.maxDepth = std::max(stats_.maxDepth, depth);
    BVHNode& node = nodes_[idx];
    if (node.count <= 1) return;

    int axis = -1; float pos = 0.f;
    float splitCost = findBestSplit(node, axis, pos);
    Bounds nb; nb.mn = node.bmin; nb.mx = node.bmax;
    float leafCost = node.count * nb.area();
    // SAH: cost(split) = C_trav * A(parent) + sum(N_child * A(child)), with C_trav ~ 1 triangle test.
    splitCost += kTraversalCost * nb.area();
    // Keep a leaf if splitting doesn't pay off (and it's small enough), or no valid split exists.
    if (axis < 0 || (splitCost >= leafCost && node.count <= maxLeaf_)) return;
    if (depth >= 48) return; // keeps the GPU traversal stack (64 entries) safe

    // Partition order_ in place.
    uint32_t i = node.leftOrFirst, j = i + node.count - 1;
    while (i <= j) {
        if (centroids_[order_[i]][axis] < pos) ++i;
        else { std::swap(order_[i], order_[j]); if (j == 0) break; --j; }
    }
    uint32_t leftCount = i - node.leftOrFirst;
    if (leftCount == 0 || leftCount == node.count) return;

    uint32_t l = used_++, r = used_++;
    nodes_[l].leftOrFirst = node.leftOrFirst; nodes_[l].count = leftCount;
    nodes_[r].leftOrFirst = i;                nodes_[r].count = node.count - leftCount;
    node.leftOrFirst = l;
    node.count = 0;
    updateBounds(l); updateBounds(r);
    subdivide(l, depth + 1);
    subdivide(r, depth + 1);
}

// ---- CPU reference traversal (same math as rt_common.glsl) ----
static float rayBox(const glm::vec3& o, const glm::vec3& invD, const glm::vec3& mn, const glm::vec3& mx, float tMax) {
    glm::vec3 t0 = (mn - o) * invD, t1 = (mx - o) * invD;
    glm::vec3 tn = glm::min(t0, t1), tf = glm::max(t0, t1);
    float tnear = std::max(std::max(tn.x, tn.y), std::max(tn.z, 0.f));
    float tfar = std::min(std::min(tf.x, tf.y), std::min(tf.z, tMax));
    return tnear <= tfar ? tnear : 1e30f;
}

static float rayTri(const glm::vec3& o, const glm::vec3& d, const GpuTri& t) {
    glm::vec3 e1 = glm::vec3(t.v1) - glm::vec3(t.v0), e2 = glm::vec3(t.v2) - glm::vec3(t.v0);
    glm::vec3 p = glm::cross(d, e2);
    float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-10f) return -1.f;
    float inv = 1.f / det;
    glm::vec3 s = o - glm::vec3(t.v0);
    float u = glm::dot(s, p) * inv;
    if (u < 0.f || u > 1.f) return -1.f;
    glm::vec3 q = glm::cross(s, e1);
    float v = glm::dot(d, q) * inv;
    if (v < 0.f || u + v > 1.f) return -1.f;
    float tt = glm::dot(e2, q) * inv;
    return tt > 0.f ? tt : -1.f;
}

float BVH::intersect(const glm::vec3& o, const glm::vec3& d, float tMax, uint32_t* triOut) const {
    if (nodes_.empty()) return -1.f;
    glm::vec3 invD = 1.f / d;
    uint32_t stack[64]; int sp = 0;
    uint32_t idx = 0;
    float best = tMax; int bestTri = -1;
    if (rayBox(o, invD, nodes_[0].bmin, nodes_[0].bmax, best) >= 1e30f) return -1.f;
    for (;;) {
        const BVHNode& n = nodes_[idx];
        if (n.count) {
            for (uint32_t i = 0; i < n.count; ++i) {
                float t = rayTri(o, d, tris_[n.leftOrFirst + i]);
                if (t > 0.f && t < best) { best = t; bestTri = int(n.leftOrFirst + i); }
            }
        } else {
            uint32_t a = n.leftOrFirst, b = a + 1;
            float da = rayBox(o, invD, nodes_[a].bmin, nodes_[a].bmax, best);
            float db = rayBox(o, invD, nodes_[b].bmin, nodes_[b].bmax, best);
            if (da > db) { std::swap(da, db); std::swap(a, b); }
            if (da < 1e30f) {
                if (db < 1e30f) stack[sp++] = b;
                idx = a;
                continue;
            }
        }
        if (sp == 0) break;
        idx = stack[--sp];
    }
    if (bestTri < 0) return -1.f;
    if (triOut) *triOut = uint32_t(bestTri);
    return best;
}

} // namespace gfx::rt
