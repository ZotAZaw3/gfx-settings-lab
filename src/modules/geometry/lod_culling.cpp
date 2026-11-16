// geometry.lod — Model quality, LOD bias, draw distance, frustum / occlusion / small-object culling.
//
// Owns ALL culling of the core DrawList (core fills it with every primitive at LOD0, no culling).
//   * init/update: builds LOD chains for every scene primitive (vertex clustering, Scene::addLod)
//                  and adds the "LOD field" showcase (400 dense torus knots on the terrain).
//   * filterDraws: draw distance -> frustum -> small-object -> CPU occlusion (software-rasterized
//                  depth of the biggest occluders + terrain) -> LOD pick by projected error.
// Docs: docs/mesh_quality.md, docs/lod_draw_distance.md, docs/culling.md
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

constexpr int kMaxLods = 4;
constexpr int kOccW = 256, kOccH = 144;   // software occlusion buffer

// --------------------------------------------------------------------------------------------
// Tiny software depth rasterizer (reversed-Z: larger = nearer, cleared to 0)
// --------------------------------------------------------------------------------------------
struct OcclusionBuffer {
    std::vector<float> mips[6];
    int w[6], h[6];
    void clear() {
        w[0] = kOccW; h[0] = kOccH; mips[0].assign(size_t(kOccW) * kOccH, 0.0f);
    }
    void rasterTriangle(glm::vec4 a, glm::vec4 b, glm::vec4 c) {
        if (a.w < 1e-3f || b.w < 1e-3f || c.w < 1e-3f) return;     // crossing near plane: skip (conservative)
        auto toScreen = [&](glm::vec4 p) {
            return glm::vec3((p.x / p.w * 0.5f + 0.5f) * kOccW, (0.5f - p.y / p.w * 0.5f) * kOccH, p.z / p.w);
        };
        glm::vec3 A = toScreen(a), B = toScreen(b), C = toScreen(c);
        float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
        if (std::abs(area) < 1e-6f) return;
        int x0 = std::max(0, int(std::floor(std::min({A.x, B.x, C.x}))));
        int x1 = std::min(kOccW - 1, int(std::ceil(std::max({A.x, B.x, C.x}))));
        int y0 = std::max(0, int(std::floor(std::min({A.y, B.y, C.y}))));
        int y1 = std::min(kOccH - 1, int(std::ceil(std::max({A.y, B.y, C.y}))));
        if (x0 > x1 || y0 > y1) return;
        float inv = 1.0f / area;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((B.x - px) * (C.y - py) - (B.y - py) * (C.x - px)) * inv;
                float w1 = ((C.x - px) * (A.y - py) - (C.y - py) * (A.x - px)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * A.z + w1 * B.z + w2 * C.z;
                float& d = mips[0][size_t(y) * kOccW + x];
                d = std::max(d, z);
            }
    }
    void buildMips() {   // each mip keeps the FARTHEST (min in reversed-Z) of its 2x2 block
        for (int m = 1; m < 6; ++m) {
            w[m] = std::max(1, (w[m - 1] + 1) / 2); h[m] = std::max(1, (h[m - 1] + 1) / 2);
            mips[m].assign(size_t(w[m]) * h[m], 0.0f);
            for (int y = 0; y < h[m]; ++y)
                for (int x = 0; x < w[m]; ++x) {
                    float v = 1e9f;
                    for (int k = 0; k < 4; ++k) {
                        int sx = std::min(x * 2 + (k & 1), w[m - 1] - 1), sy = std::min(y * 2 + (k >> 1), h[m - 1] - 1);
                        v = std::min(v, mips[m - 1][size_t(sy) * w[m - 1] + sx]);
                    }
                    mips[m][size_t(y) * w[m] + x] = v;
                }
        }
    }
    // true if the AABB is certainly hidden
    bool occluded(const glm::mat4& vp, const glm::vec3& mn, const glm::vec3& mx) const {
        glm::vec2 smin(1e9f), smax(-1e9f); float nearest = 0.0f;
        for (int i = 0; i < 8; ++i) {
            glm::vec4 p = vp * glm::vec4((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z, 1.0f);
            if (p.w < 1e-3f) return false;                    // touches the camera plane: visible
            glm::vec2 s((p.x / p.w * 0.5f + 0.5f) * kOccW, (0.5f - p.y / p.w * 0.5f) * kOccH);
            smin = glm::min(smin, s); smax = glm::max(smax, s);
            nearest = std::max(nearest, p.z / p.w);
        }
        smin = glm::max(smin, glm::vec2(0)); smax = glm::min(smax, glm::vec2(kOccW - 1, kOccH - 1));
        if (smin.x > smax.x || smin.y > smax.y) return false;
        float size = std::max(smax.x - smin.x, smax.y - smin.y);
        int m = std::clamp(int(std::ceil(std::log2(std::max(size, 1.0f) / 2.0f))), 0, 5);
        int x0 = int(smin.x) >> m, x1 = int(smax.x) >> m, y0 = int(smin.y) >> m, y1 = int(smax.y) >> m;
        float farthest = 1e9f;
        for (int y = y0; y <= std::min(y1, h[m] - 1); ++y)
            for (int x = x0; x <= std::min(x1, w[m] - 1); ++x) farthest = std::min(farthest, mips[m][size_t(y) * w[m] + x]);
        return nearest < farthest;   // every occluder texel is nearer than the object's nearest point
    }
};

class LodCullingModule : public Module {
public:
    const char* name() const override { return "geometry.lod"; }

    void registerSettings(SettingsRegistry& s) override {
        meshQuality_ = &s.addEnum({.id = "geometry.meshQuality", .label = "Model Quality", .category = "Geometry",
                                   .group = "Models & LOD",
                                   .tooltip = "Starting level of detail for every model (a global LOD offset).",
                                   .doc = "mesh_quality"},
                                  {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3);
        lodBias_ = &s.addInt({.id = "geometry.lodBias", .label = "LOD Bias", .category = "Geometry", .group = "Models & LOD",
                              .tooltip = "Negative = keep detailed models longer, positive = switch to simpler models sooner.",
                              .doc = "lod_draw_distance"}, 0, -2, 3).presets(1, 0, 0, -1);
        lodHysteresis_ = &s.addBool({.id = "geometry.lodHysteresis", .label = "LOD Hysteresis", .category = "Geometry",
                                     .group = "Models & LOD",
                                     .tooltip = "Requires a margin before switching back, stops LOD flicker at thresholds.",
                                     .doc = "lod_draw_distance", .flags = SettingNoPreset}, true);
        drawDistance_ = &s.addFloat({.id = "geometry.drawDistance", .label = "Draw Distance", .category = "Geometry",
                                     .group = "Models & LOD", .tooltip = "Objects farther than this are not drawn (metres).",
                                     .doc = "lod_draw_distance"}, 600.0f, 50.0f, 2000.0f).presets(150, 300, 600, 1200).fmt("%.0f m");
        frustum_ = &s.addBool({.id = "performance.frustumCulling", .label = "Frustum Culling", .category = "Performance",
                               .group = "Culling", .tooltip = "Skip objects outside the camera's view.",
                               .doc = "culling", .flags = SettingNoPreset}, true);
        occlusion_ = &s.addEnum({.id = "performance.occlusionCulling", .label = "Occlusion Culling", .category = "Performance",
                                 .group = "Culling",
                                 .tooltip = "Skip objects hidden behind others (CPU software-rasterized depth of big occluders).",
                                 .doc = "culling"}, {"Off", "On"}, 1).presets(1, 1, 1, 1);
        smallPx_ = &s.addFloat({.id = "performance.smallObjectCulling", .label = "Small Object Culling", .category = "Performance",
                                .group = "Culling", .tooltip = "Skip objects smaller than this many pixels on screen (0 = off).",
                                .doc = "culling"}, 1.0f, 0.0f, 8.0f).presets(4.0f, 2.0f, 1.0f, 0.5f).fmt("%.1f px");
        showcase_ = &s.addBool({.id = "world.showcase", .label = "Geometry Showcase Area", .category = "World", .group = "Showcase",
                                .tooltip = "Terrain, water, grass, LOD field, particles... around x 20..180, z -20..-180.",
                                .doc = "lod_draw_distance", .flags = SettingNoPreset}, true);
        camPreset_ = &s.addEnum({.id = "world.showcaseCamera", .label = "Jump Camera To", .category = "World", .group = "Showcase",
                                 .tooltip = "Moves the camera to a showcase viewpoint (then fly freely).",
                                 .doc = "lod_draw_distance", .flags = SettingNoPreset},
                                {"(stay)", "Plaza: particles, decals, fur, cloth, crowd", "Terrain overview", "LOD field",
                                 "Lake + grass", "Fur + cloth close-up"}, 0)
                         .changed([this](Setting& st) { pendingCam_ = st.asInt(); });
    }

    void init(Engine& e) override {
        prepareScene(e);
        pendingCam_ = camPreset_->asInt();   // value loaded from settings.ini
    }

    void update(FrameContext& ctx) override {
        // Scene can be swapped at runtime (scene picker): rebuild LODs / showcase when geometry changes.
        if (ctx.scene.geometryVersion() != builtVersion_) prepareScene(ctx.engine);
        if (pendingCam_ > 0) {
            static const glm::vec3 kFrom[6] = {{}, {58, 5, -26}, {20, 45, -20}, {62, 14, -150}, {80, 4, -104}, {41, 2.5f, -38}};
            static const glm::vec3 kTo[6] = {{}, {42, 1, -42}, {100, 5, -100}, {120, 4, -110}, {110, 1, -110}, {33, 2.5f, -40}};
            glm::vec3 from = kFrom[pendingCam_];
            // never start inside a hill: keep the camera a few metres above the terrain
            from.y = std::max(from.y, geo::terrainHeightWorld(from.x, from.z, 30.0f) + 3.0f);
            glm::vec3 d = glm::normalize(kTo[pendingCam_] - from);
            ctx.camera.position = from;
            ctx.camera.yaw = std::atan2(d.z, d.x);
            ctx.camera.pitch = std::asin(d.y);
            pendingCam_ = 0;
        }
    }

    void filterDraws(FrameContext& ctx, DrawList& list) override {
        const FrameConstants& fc = ctx.frame;
        const glm::mat4& vp = fc.unjitteredViewProj;
        const geo::Frustum fr = geo::extractFrustum(vp);
        const glm::vec3 cam(fc.cameraPos);
        const float pxScale = geo::projScaleY(fc);
        // Model quality = global LOD offset + error tolerance: Low +2 levels, Medium +1, High 0, Ultra 0 with half tolerance
        static const int kBase[4] = {2, 1, 0, 0};
        static const float kTol[4] = {1.5f, 1.25f, 1.0f, 0.5f};
        const int q = std::clamp(meshQuality_->asInt(), 0, 3);
        const float tolPx = kTol[q] * std::pow(2.0f, float(lodBias_->asInt()));   // allowed projected error (px)
        const int baseLod = kBase[q];
        const float drawDist = drawDistance_->asFloat();
        const bool showcase = showcase_->asBool();
        Scene& sc = ctx.scene;

        uint32_t cDist = 0, cFrustum = 0, cSmall = 0, cOcc = 0, cShowcase = 0;
        uint64_t tris = 0;

        struct Cand { DrawItem item; float radiusPx; };
        auto classify = [&](std::vector<DrawItem>& items, std::vector<Cand>& out) {
            for (DrawItem& d : items) {
                const Instance& inst = sc.instances[d.instance];
                if (!showcase && inst.mesh == fieldMesh_ && fieldMesh_ != ~0u) { ++cShowcase; continue; }
                const Primitive& prim = sc.primitives[d.primitive];
                AABB wb = prim.bounds.transformed(inst.world);
                glm::vec3 c = wb.center(); float r = glm::length(wb.extent());
                float dist = glm::length(c - cam);
                d.distance = dist;
                if (dist - r > drawDist) { ++cDist; continue; }
                if (frustum_->asBool() && !geo::aabbInFrustum(fr, wb.min, wb.max)) { ++cFrustum; continue; }
                float rpx = (dist > r) ? r / dist * pxScale : 1e6f;
                if (smallPx_->asFloat() > 0.0f && rpx * 2.0f < smallPx_->asFloat()) { ++cSmall; continue; }
                out.push_back({d, rpx});
            }
        };
        std::vector<Cand> opq, trn;
        classify(list.opaque, opq);
        classify(list.transparent, trn);

        // ---- CPU occlusion: rasterize the biggest visible occluders (coarsest LOD) + terrain
        bool occ = occlusion_->asInt() == 1;
        if (occ) {
            occBuf_.clear();
            std::vector<const Cand*> big;
            for (auto& c : opq) if (c.radiusPx > 40.0f) big.push_back(&c);
            std::sort(big.begin(), big.end(), [](auto* a, auto* b) { return a->radiusPx > b->radiusPx; });
            if (big.size() > 48) big.resize(48);
            for (const Cand* c : big) {
                const Primitive& prim = sc.primitives[c->item.primitive];
                const LodLevel& L = prim.lods.back();
                if (L.indexCount > 6000) continue;
                glm::mat4 m = vp * sc.instances[c->item.instance].world;
                for (uint32_t i = 0; i + 2 < L.indexCount; i += 3) {
                    auto P = [&](uint32_t k) { return m * glm::vec4(sc.vertices[prim.baseVertex + sc.indices[L.firstIndex + i + k]].position, 1.0f); };
                    occBuf_.rasterTriangle(P(0), P(1), P(2));
                }
            }
            if (showcase) rasterTerrain(vp, ctx);
            occBuf_.buildMips();
        }

        auto finish = [&](std::vector<Cand>& cands, std::vector<DrawItem>& out) {
            out.clear();
            for (Cand& c : cands) {
                const Instance& inst = sc.instances[c.item.instance];
                const Primitive& prim = sc.primitives[c.item.primitive];
                if (occ && c.radiusPx < 1e5f) {
                    AABB wb = prim.bounds.transformed(inst.world);
                    if (occBuf_.occluded(vp, wb.min, wb.max)) { ++cOcc; continue; }
                }
                // LOD: coarsest level whose projected simplification error stays under tolPx
                float scale = std::cbrt(std::abs(glm::determinant(glm::mat3(inst.world))));
                int nl = std::min<int>(int(prim.lods.size()), kMaxLods);
                int lod = 0;
                uint64_t key = (uint64_t(c.item.instance) << 32) | c.item.primitive;
                int prev = lastLod_.count(key) ? lastLod_[key] : -1;
                for (int l = nl - 1; l > 0; --l) {
                    float errPx = prim.lods[l].error * scale / std::max(c.item.distance, 0.01f) * pxScale;
                    // hysteresis: going coarser than last frame needs 20 % margin
                    float tol = (lodHysteresis_->asBool() && prev >= 0 && l > prev) ? tolPx * 0.8f : tolPx;
                    if (errPx <= tol) { lod = l; break; }
                }
                lod = std::clamp(lod + std::max(baseLod, 0), 0, nl - 1);
                lastLod_[key] = lod;
                c.item.lod = uint32_t(lod);
                tris += prim.lods[lod].indexCount / 3;
                out.push_back(c.item);
            }
        };
        finish(opq, list.opaque);
        finish(trn, list.transparent);

        list.culled = cDist + cFrustum + cSmall + cOcc + cShowcase;
        ctx.profiler.counter("cull.distance", cDist);
        ctx.profiler.counter("cull.frustum", cFrustum);
        ctx.profiler.counter("cull.small", cSmall);
        ctx.profiler.counter("cull.occlusion", cOcc);
        ctx.profiler.counter("lod.triangles", double(tris));
        if (lastLod_.size() > 200000) lastLod_.clear();
    }

    size_t cpuMemoryBytes() const override {
        size_t b = lastLod_.size() * 24;
        for (auto& m : occBuf_.mips) b += m.size() * sizeof(float);
        return b;
    }

private:
    void rasterTerrain(const glm::mat4& vp, FrameContext& ctx) {
        // coarse 48x48 grid of the heightfield as occluder (the terrain itself is drawn by geometry.terrain)
        Setting* dispS = ctx.settings.find("world.terrainDisplacement");
        float disp = dispS ? dispS->asFloat() * 30.0f : 30.0f;
        const int N = 48;
        auto P = [&](int x, int z) {
            float wx = geo::kTerrainCenter.x + (float(x) / N - 0.5f) * geo::kTerrainSize;
            float wz = geo::kTerrainCenter.z + (float(z) / N - 0.5f) * geo::kTerrainSize;
            // lower the occluder slightly so coarse triangles never poke above the real surface
            return vp * glm::vec4(wx, geo::terrainHeightWorld(wx, wz, disp) - 1.0f, wz, 1.0f);
        };
        for (int z = 0; z < N; ++z)
            for (int x = 0; x < N; ++x) {
                glm::vec4 a = P(x, z), b = P(x + 1, z), c = P(x, z + 1), d = P(x + 1, z + 1);
                occBuf_.rasterTriangle(a, b, c); occBuf_.rasterTriangle(b, d, c);
            }
    }

    void prepareScene(Engine& e) {
        Scene& sc = e.scene();
        addShowcaseField(e);
        // LOD chains for every primitive that only has LOD0
        bool changed = false;
        for (uint32_t p = 0; p < sc.primitives.size(); ++p) {
            Primitive& prim = sc.primitives[p];
            if (prim.lods.size() != 1 || prim.lods[0].indexCount < 600) continue;
            std::vector<uint32_t> base(sc.indices.begin() + prim.lods[0].firstIndex,
                                       sc.indices.begin() + prim.lods[0].firstIndex + prim.lods[0].indexCount);
            size_t prevCount = base.size();
            for (int cells : {40, 20, 10}) {
                float err = 0.0f;
                auto lod = geo::simplifyClustered(&sc.vertices[prim.baseVertex].position, prim.vertexCount, sizeof(Vertex),
                                                  base, cells, &err);
                if (lod.size() < 12 || lod.size() > prevCount * 85 / 100) continue;
                sc.addLod(p, lod, err);
                prevCount = lod.size();
                changed = true;
            }
        }
        if (changed) { sc.markGeometryDirty(); sc.upload(e.device()); }
        builtVersion_ = sc.geometryVersion();
    }

    void addShowcaseField(Engine& e) {
        Scene& sc = e.scene();
        for (uint32_t m = 0; m < sc.meshes.size(); ++m)
            if (sc.meshes[m].name == "geo.lodfield") { fieldMesh_ = m; return; }
        // one dense torus knot (Ultra detail); LOD 1..3 are generated by prepareScene's simplifier
        geo::SimpleMesh knot = geo::makeTorusKnot(384, 20, 1.0f, 0.28f);
        Primitive prim;
        prim.baseVertex = uint32_t(sc.vertices.size());
        prim.vertexCount = uint32_t(knot.vertices.size());
        for (auto& v : knot.vertices) {
            glm::vec3 t = glm::normalize(glm::cross(std::abs(v.normal.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), v.normal));
            sc.vertices.push_back({v.pos, v.normal, glm::vec4(t, 1.0f), v.uv});
            prim.bounds.expand(v.pos);
        }
        LodLevel l0; l0.firstIndex = uint32_t(sc.indices.size()); l0.indexCount = uint32_t(knot.indices.size());
        sc.indices.insert(sc.indices.end(), knot.indices.begin(), knot.indices.end());
        prim.lods.push_back(l0);
        Material mat; mat.name = "geo.lodfield"; mat.baseColor = glm::vec4(0.75f, 0.55f, 0.3f, 1.0f);
        mat.roughness = 0.35f; mat.metallic = 1.0f;
        sc.materials.push_back(mat);
        prim.material = int(sc.materials.size() - 1);
        sc.primitives.push_back(prim);
        Mesh mesh; mesh.name = "geo.lodfield"; mesh.primitives.push_back(uint32_t(sc.primitives.size() - 1));
        sc.meshes.push_back(mesh);
        fieldMesh_ = uint32_t(sc.meshes.size() - 1);
        // 20 x 20 grid on the terrain, skipping the plaza and the lake
        for (int z = 0; z < 20; ++z)
            for (int x = 0; x < 20; ++x) {
                float wx = geo::kTerrainCenter.x + (x - 9.5f) * 7.5f, wz = geo::kTerrainCenter.z + (z - 9.5f) * 7.5f;
                if (glm::length(glm::vec2(wx - geo::kPlaza.x, wz - geo::kPlaza.z)) < geo::kPlazaRadius + 4.0f) continue;
                if (glm::length(glm::vec2(wx - geo::kLake.x, wz - geo::kLake.z)) < geo::kLakeRadius + 2.0f) continue;
                Instance inst;
                float y = geo::terrainHeightWorld(wx, wz, 30.0f) + 1.6f;
                inst.world = glm::translate(glm::mat4(1.0f), glm::vec3(wx, y, wz)) *
                             glm::rotate(glm::mat4(1.0f), float(x * 7 + z * 13), glm::vec3(0, 1, 0));
                inst.prevWorld = inst.world;
                inst.mesh = fieldMesh_;
                sc.instances.push_back(inst);
            }
        sc.updateInstances();
        sc.markMaterialsDirty();
        sc.markGeometryDirty();
        sc.upload(e.device());
    }

    Setting *meshQuality_ = nullptr, *lodBias_ = nullptr, *lodHysteresis_ = nullptr, *drawDistance_ = nullptr;
    Setting *frustum_ = nullptr, *occlusion_ = nullptr, *smallPx_ = nullptr, *showcase_ = nullptr, *camPreset_ = nullptr;
    int pendingCam_ = 0;
    uint64_t builtVersion_ = 0;
    uint32_t fieldMesh_ = ~0u;
    std::unordered_map<uint64_t, int> lastLod_;
    OcclusionBuffer occBuf_;
};

}  // namespace

GFX_REGISTER_MODULE(LodCullingModule);
