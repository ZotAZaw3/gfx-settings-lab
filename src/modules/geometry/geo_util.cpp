#include "modules/geometry/geo_util.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace geo {
using namespace gfx;

static constexpr float kPi = 3.14159265358979f;

// ------------------------------------------------------------------------------------------
// Procedural meshes
// ------------------------------------------------------------------------------------------
SimpleMesh makeSphere(int seg, int rings, float r) {
    SimpleMesh m;
    for (int y = 0; y <= rings; ++y) {
        float v = float(y) / rings, th = v * kPi;
        for (int x = 0; x <= seg; ++x) {
            float u = float(x) / seg, ph = u * 2.0f * kPi;
            glm::vec3 n(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
            m.vertices.push_back({n * r, n, {u, v}});
        }
    }
    for (int y = 0; y < rings; ++y)
        for (int x = 0; x < seg; ++x) {
            uint32_t a = y * (seg + 1) + x, b = a + seg + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    return m;
}

SimpleMesh makeTorusKnot(int seg, int sides, float R, float tube, int p, int q) {
    SimpleMesh m;
    auto curve = [&](float t) {
        float r = R * (2.0f + std::cos(q * t)) * 0.5f;
        return glm::vec3(r * std::cos(p * t), R * std::sin(q * t) * 0.5f, r * std::sin(p * t));
    };
    for (int i = 0; i <= seg; ++i) {
        float t = float(i) / seg * 2.0f * kPi;
        glm::vec3 c = curve(t), c2 = curve(t + 0.001f);
        glm::vec3 T = glm::normalize(c2 - c), B = glm::normalize(glm::cross(T, c2 + c)), N = glm::cross(B, T);
        for (int j = 0; j <= sides; ++j) {
            float a = float(j) / sides * 2.0f * kPi;
            glm::vec3 n = std::cos(a) * N + std::sin(a) * B;
            m.vertices.push_back({c + n * tube, n, {float(i) / seg * 8.0f, float(j) / sides}});
        }
    }
    for (int i = 0; i < seg; ++i)
        for (int j = 0; j < sides; ++j) {
            uint32_t a = i * (sides + 1) + j, b = a + sides + 1;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return m;
}

SimpleMesh makeCapsule(int seg, float r, float h) {
    SimpleMesh m = makeSphere(seg, seg / 2 * 2, r);
    for (auto& v : m.vertices) v.pos.y += (v.pos.y > 0.0f ? h * 0.5f : -h * 0.5f) + h * 0.5f + r;
    return m;
}

SimpleMesh makeBox() {
    SimpleMesh m;
    const glm::vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        glm::vec3 N = n[f], U = std::abs(N.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0), V = glm::cross(N, U);
        uint32_t base = uint32_t(m.vertices.size());
        for (int k = 0; k < 4; ++k) {
            float su = (k == 1 || k == 2) ? 0.5f : -0.5f, sv = (k >= 2) ? 0.5f : -0.5f;
            m.vertices.push_back({N * 0.5f + U * su + V * sv, N, {su + 0.5f, sv + 0.5f}});
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

SimpleMesh makeGridXZ(int n, float size) {
    SimpleMesh m;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            glm::vec2 uv(float(x) / n, float(z) / n);
            m.vertices.push_back({{(uv.x - 0.5f) * size, 0.0f, (uv.y - 0.5f) * size}, {0, 1, 0}, uv});
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            uint32_t a = z * (n + 1) + x, b = a + n + 1;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return m;
}

void computeNormals(SimpleMesh& m) {
    for (auto& v : m.vertices) v.normal = glm::vec3(0.0f);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        auto& a = m.vertices[m.indices[i]]; auto& b = m.vertices[m.indices[i + 1]]; auto& c = m.vertices[m.indices[i + 2]];
        glm::vec3 n = glm::cross(b.pos - a.pos, c.pos - a.pos);
        a.normal += n; b.normal += n; c.normal += n;
    }
    for (auto& v : m.vertices) v.normal = glm::length(v.normal) > 0 ? glm::normalize(v.normal) : glm::vec3(0, 1, 0);
}

// ------------------------------------------------------------------------------------------
// Vertex clustering simplifier (Rossignac–Borrel). Fast, robust, no dependencies.
// Quality is lower than quadric edge collapse (meshoptimizer), but fine for LOD 1..3.
// ------------------------------------------------------------------------------------------
std::vector<uint32_t> simplifyClustered(const glm::vec3* positions, size_t vertexCount, size_t stride,
                                        const std::vector<uint32_t>& indices, int cells, float* outError) {
    auto P = [&](size_t i) -> const glm::vec3& {
        return *reinterpret_cast<const glm::vec3*>(reinterpret_cast<const uint8_t*>(positions) + i * stride);
    };
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (uint32_t i : indices) { mn = glm::min(mn, P(i)); mx = glm::max(mx, P(i)); }
    float extent = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z, 1e-6f});
    float cell = extent / float(std::max(cells, 1));
    if (outError) *outError = cell;

    // cell -> representative vertex: the vertex closest to the cell's average position
    std::unordered_map<uint64_t, uint32_t> rep;
    std::unordered_map<uint64_t, glm::vec4> avg;
    std::vector<uint64_t> keyOf(vertexCount, ~0ull);
    auto keyFor = [&](const glm::vec3& p) {
        glm::ivec3 c = glm::ivec3(glm::floor((p - mn) / cell));
        return (uint64_t(uint32_t(c.x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(c.y) & 0x1FFFFF) << 21) | uint64_t(uint32_t(c.z) & 0x1FFFFF);
    };
    for (uint32_t i : indices) {
        if (keyOf[i] != ~0ull) continue;
        uint64_t k = keyFor(P(i)); keyOf[i] = k;
        avg[k] += glm::vec4(P(i), 1.0f);
    }
    for (auto& [k, a] : avg) a /= a.w;
    for (uint32_t i : indices) {
        uint64_t k = keyOf[i];
        auto it = rep.find(k);
        glm::vec3 center(avg[k]);
        if (it == rep.end()) rep[k] = i;
        else if (glm::distance(P(i), center) < glm::distance(P(it->second), center)) it->second = i;
    }
    std::vector<uint32_t> out;
    out.reserve(indices.size() / 2);
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        uint32_t a = rep[keyOf[indices[t]]], b = rep[keyOf[indices[t + 1]]], c = rep[keyOf[indices[t + 2]]];
        if (a == b || b == c || a == c) continue;
        out.insert(out.end(), {a, b, c});
    }
    return out;
}

// ------------------------------------------------------------------------------------------
// Frustum (Gribb–Hartmann on a D3D-style reversed-Z infinite projection)
// ------------------------------------------------------------------------------------------
Frustum extractFrustum(const glm::mat4& m) {
    // glm is column-major: row i = (m[0][i], m[1][i], m[2][i], m[3][i])
    auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    Frustum f;
    f.planes[0] = r3 + r0;   // left
    f.planes[1] = r3 - r0;   // right
    f.planes[2] = r3 + r1;   // bottom
    f.planes[3] = r3 - r1;   // top
    f.planes[4] = r3 - r2;   // near (reversed-Z: z <= w); the far plane is at infinity
    for (auto& p : f.planes) p /= glm::length(glm::vec3(p));
    return f;
}
bool sphereInFrustum(const Frustum& f, const glm::vec3& c, float r) {
    for (const auto& p : f.planes)
        if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
    return true;
}
bool aabbInFrustum(const Frustum& f, const glm::vec3& mn, const glm::vec3& mx) {
    for (const auto& p : f.planes) {
        glm::vec3 v(p.x >= 0 ? mx.x : mn.x, p.y >= 0 ? mx.y : mn.y, p.z >= 0 ? mx.z : mn.z);   // positive vertex
        if (glm::dot(glm::vec3(p), v) + p.w < 0.0f) return false;
    }
    return true;
}
float projScaleY(const FrameConstants& fc) { return std::abs(fc.unjitteredProj[1][1]) * fc.renderSize.y * 0.5f; }

// ------------------------------------------------------------------------------------------
// Terrain heightfield: hills + a flat plaza + a lake basin. Normalized 0..1.
// ------------------------------------------------------------------------------------------
static float vnoise(float x, float y) {
    auto h = [](int ix, int iy) {
        uint32_t n = uint32_t(ix) * 374761393u + uint32_t(iy) * 668265263u;
        n = (n ^ (n >> 13)) * 1274126177u; return float(n & 0xFFFFFF) / float(0xFFFFFF);
    };
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    float a = h(ix, iy), b = h(ix + 1, iy), c = h(ix, iy + 1), d = h(ix + 1, iy + 1);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
void HeightField::generate(int r) {
    res = r; h.assign(size_t(r) * r, 0.0f);
    for (int y = 0; y < r; ++y)
        for (int x = 0; x < r; ++x) {
            float wx = kTerrainCenter.x + (float(x) / (r - 1) - 0.5f) * kTerrainSize;
            float wz = kTerrainCenter.z + (float(y) / (r - 1) - 0.5f) * kTerrainSize;
            float s = 0, a = 0.5f, fq = 0.02f;
            for (int o = 0; o < 6; ++o) { s += a * vnoise(wx * fq, wz * fq); fq *= 2.07f; a *= 0.5f; }
            float ridge = 1.0f - std::abs(vnoise(wx * 0.012f + 7.0f, wz * 0.012f) * 2.0f - 1.0f);
            float v = std::pow(s, 1.6f) * 0.8f + ridge * ridge * 0.35f;
            float dp = glm::length(glm::vec2(wx - kPlaza.x, wz - kPlaza.z));
            v *= glm::smoothstep(kPlazaRadius, kPlazaRadius + 15.0f, dp);           // flatten plaza
            float dl = glm::length(glm::vec2(wx - kLake.x, wz - kLake.z));
            float basin = 1.0f - glm::smoothstep(kLakeRadius * 0.4f, kLakeRadius * 1.2f, dl);
            v = glm::mix(v, -0.12f, basin);                                         // lake basin below 0
            h[size_t(y) * r + x] = v;
        }
}
float HeightField::sample(float wx, float wz) const {
    if (res == 0) return 0.0f;
    float u = ((wx - kTerrainCenter.x) / kTerrainSize + 0.5f) * (res - 1);
    float v = ((wz - kTerrainCenter.z) / kTerrainSize + 0.5f) * (res - 1);
    u = glm::clamp(u, 0.0f, float(res - 1)); v = glm::clamp(v, 0.0f, float(res - 1));
    int x0 = int(u), y0 = int(v), x1 = std::min(x0 + 1, res - 1), y1 = std::min(y0 + 1, res - 1);
    float fx = u - x0, fy = v - y0;
    auto H = [&](int x, int y) { return h[size_t(y) * res + x]; };
    return glm::mix(glm::mix(H(x0, y0), H(x1, y0), fx), glm::mix(H(x0, y1), H(x1, y1), fx), fy);
}
const HeightField& terrainHeights() {
    static HeightField hf = [] { HeightField f; f.generate(512); return f; }();
    return hf;
}
float terrainHeightWorld(float x, float z, float disp) { return terrainHeights().sample(x, z) * disp; }

// ------------------------------------------------------------------------------------------
// GPU helpers
// ------------------------------------------------------------------------------------------
rhi::BufferPtr makeBuffer(Engine& e, size_t bytes, uint32_t usage, const char* name, const char* owner,
                          const void* data, uint32_t stride) {
    rhi::BufferDesc d;
    d.size = bytes; d.usage = usage; d.dynamic = (usage & rhi::BufUsageUniform) != 0;
    d.structStride = stride; d.name = name; d.owner = owner;
    return e.device().createBuffer(d, data);
}
rhi::VertexLayout simpleVertexLayout() {
    rhi::VertexLayout l;
    l.attribs = {{0, rhi::VertexFormat::Float3, 0}, {1, rhi::VertexFormat::Float3, 12}, {3, rhi::VertexFormat::Float2, 24}};
    l.strides[0] = sizeof(SimpleVertex);
    return l;
}

void beginGBufferPass(FrameContext& ctx, const char* name) {
    rhi::RenderPassDesc rp;
    const char* names[5] = {"gbuffer.albedo", "gbuffer.normal", "gbuffer.material", "gbuffer.emissive", "scene.velocity"};
    for (int i = 0; i < 5; ++i) { rp.colors[i].texture = ctx.targets.get(names[i]); rp.colors[i].load = rhi::LoadOp::Load; }
    rp.colorCount = 5;
    rp.depth.texture = ctx.targets.get("scene.depth");
    rp.depth.load = rhi::LoadOp::Load;
    rp.name = name;
    ctx.cmd.beginRenderPass(rp);
}
rhi::PipelineDesc gbufferPipelineDesc(rhi::Program* prog, const char* name) {
    rhi::PipelineDesc d;
    d.program = prog; d.name = name;
    d.layout = simpleVertexLayout();
    d.raster.cull = rhi::CullMode::None;   // procedural meshes: winding not guaranteed, keep it simple
    d.depth = {true, true, rhi::CompareOp::Greater};
    return d;
}

}  // namespace geo
