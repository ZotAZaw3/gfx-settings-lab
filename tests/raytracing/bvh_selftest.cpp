// Compares BVH traversal against brute force on a random triangle soup.
#include "../src/modules/raytracing/bvh.h"
#include <cstdio>
#include <random>
#include <cmath>
using namespace gfx::rt;
int main() {
    std::mt19937 rng(42); std::uniform_real_distribution<float> U(-10.f, 10.f), S(-0.5f, 0.5f);
    std::vector<BuildTriangle> tris(20000);
    for (auto& t : tris) { glm::vec3 c(U(rng), U(rng), U(rng));
        for (int k = 0; k < 3; ++k) { t.p[k] = c + glm::vec3(S(rng), S(rng), S(rng)); t.n[k] = glm::vec3(0,1,0); }
        t.material = 0; }
    BVH bvh; bvh.build(tris);
    auto& st = bvh.stats();
    std::printf("tris %u nodes %u leaves %u depth %u build %.2f ms\n", st.triangles, st.nodes, st.leaves, st.maxDepth, st.buildMs);
    int mismatches = 0, hits = 0;
    for (int r = 0; r < 2000; ++r) {
        glm::vec3 o(U(rng), U(rng), U(rng)), d = glm::normalize(glm::vec3(U(rng), U(rng), U(rng)));
        float bvhT = bvh.intersect(o, d, 1e30f);
        float bf = -1.f;
        for (auto& g : bvh.gpuTris()) { // brute force with same tri test
            glm::vec3 v0(g.v0), e1 = glm::vec3(g.v1) - v0, e2 = glm::vec3(g.v2) - v0, p = glm::cross(d, e2);
            float det = glm::dot(e1, p); if (std::fabs(det) < 1e-10f) continue; float inv = 1.f / det;
            glm::vec3 s = o - v0; float u = glm::dot(s, p) * inv; if (u < 0 || u > 1) continue;
            glm::vec3 q = glm::cross(s, e1); float v = glm::dot(d, q) * inv; if (v < 0 || u + v > 1) continue;
            float t = glm::dot(e2, q) * inv; if (t > 0 && (bf < 0 || t < bf)) bf = t; }
        if (bf > 0) hits++;
        if ((bf < 0) != (bvhT < 0) || (bf > 0 && std::fabs(bf - bvhT) > 1e-4f)) mismatches++;
    }
    std::printf("rays 2000 hits %d mismatches %d\n", hits, mismatches);
    return mismatches ? 1 : 0;
}
