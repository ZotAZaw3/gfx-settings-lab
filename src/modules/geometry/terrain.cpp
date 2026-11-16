// geometry.terrain — Tessellation + displacement, Terrain quality, Parallax occlusion mapping.
//
//   GBuffer stage, order 10: "Terrain (tessellated)" or "Terrain (grid)" when tessellation is Off.
//   GBuffer stage, order 12: "POM floor" — a 12 x 12 m brick floor on the plaza.
// Docs: docs/tessellation.md, docs/terrain_quality.md, docs/parallax_occlusion_mapping.md
#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "core/engine.h"
#include "modules/geometry/geo_util.h"

namespace {
using namespace gfx;

struct TerrainParams {            // std140, mirrors shaders/geometry/terrain_params.glsl
    glm::vec4 a;   // size, displacement m, target px, max level
    glm::vec4 b;   // proj scale, cull patches, layers, triplanar
    glm::vec4 c;   // center xz, show triangles, -
};
struct PomParams {                // std140, mirrors shaders/geometry/pom_params.glsl
    glm::mat4 model;
    glm::vec4 T, B, N;
    glm::vec4 p0;  // mode, depth, min layers, max layers
    glm::vec4 p1;  // refine steps, shadow steps, uv tiling, -
};

class TerrainModule : public Module {
public:
    const char* name() const override { return "geometry.terrain"; }

    void registerSettings(SettingsRegistry& s) override {
        tess_ = &s.addEnum({.id = "geometry.tessellation", .label = "Tessellation", .category = "Geometry",
                            .group = "Surface Detail",
                            .tooltip = "GPU subdivides terrain patches and displaces them with a height map.",
                            .doc = "tessellation"},
                           {"Off", "Low", "Medium", "High", "Ultra"}, 3).presets(0, 2, 3, 4)
                   .enabled([this] { return tessSupported_; });
        disp_ = &s.addFloat({.id = "world.terrainDisplacement", .label = "Displacement Strength", .category = "Geometry",
                             .group = "Surface Detail", .tooltip = "Height of the displaced terrain (1 = 30 m hills).",
                             .doc = "tessellation", .flags = SettingNoPreset}, 1.0f, 0.0f, 1.0f);
        tessCull_ = &s.addBool({.id = "geometry.tessPatchCulling", .label = "Patch Frustum Culling", .category = "Geometry",
                                .group = "Surface Detail", .tooltip = "Hull shader drops patches outside the view (tess factor 0).",
                                .doc = "tessellation", .flags = SettingNoPreset | SettingDebug}, true);
        showTris_ = &s.addBool({.id = "geometry.showTriangles", .label = "Show Terrain Triangles", .category = "Geometry",
                                .group = "Surface Detail", .tooltip = "Facet shading so you can see the tessellated triangles.",
                                .doc = "tessellation", .flags = SettingNoPreset | SettingDebug}, false);
        terrainQ_ = &s.addEnum({.id = "world.terrainQuality", .label = "Terrain Quality", .category = "World",
                                .group = "Terrain",
                                .tooltip = "Patch density, tessellation cap and material layers of the ground.",
                                .doc = "terrain_quality"}, {"Low", "Medium", "High", "Ultra"}, 2).presets(0, 1, 2, 3);
        pom_ = &s.addEnum({.id = "geometry.pom", .label = "Parallax Occlusion Mapping", .category = "Geometry",
                           .group = "Surface Detail",
                           .tooltip = "Ray-marches a height map per pixel so flat surfaces look deep (bricks, cobbles).",
                           .doc = "parallax_occlusion_mapping"}, {"Off", "Low", "Medium", "High"}, 2).presets(0, 1, 2, 3);
        pomDepth_ = &s.addFloat({.id = "geometry.pomDepth", .label = "POM Depth", .category = "Geometry", .group = "Surface Detail",
                                 .tooltip = "Apparent depth of the parallax surface (in UV units).",
                                 .doc = "parallax_occlusion_mapping", .flags = SettingNoPreset}, 0.06f, 0.0f, 0.15f)
                       .visible([this] { return pom_->asInt() > 0; });
    }

    void init(Engine& e) override {
        tessSupported_ = e.device().caps().tessellation;
        auto& hf = geo::terrainHeights();
        rhi::TextureDesc td; td.format = rhi::Format::R32F; td.width = td.height = uint32_t(hf.res);
        td.name = "terrain.height"; td.owner = name();
        height_ = e.device().createTexture(td, hf.h.data());
        rhi::SamplerDesc sd; sd.addressU = sd.addressV = rhi::AddressMode::Clamp; sd.useMips = false;
        clampLinear_ = e.device().createSampler(sd);

        buildPatchBuffers(e);

        rhi::ProgramDesc pt; pt.vs = "geometry/terrain.vert"; pt.tcs = "geometry/terrain.tesc"; pt.tes = "geometry/terrain.tese";
        pt.fs = "geometry/terrain.frag"; pt.name = "terrain.tess";
        rhi::PipelineDesc d; d.layout.attribs = {{0, rhi::VertexFormat::Float3, 0}}; d.layout.strides[0] = 12;
        d.raster.cull = rhi::CullMode::None; d.depth = {true, true, rhi::CompareOp::Greater};
        if (tessSupported_) {
            d.program = e.shaders().get(pt); d.topology = rhi::Topology::Patches; d.patchControlPoints = 4; d.name = "terrain.tess";
            tessPipe_ = e.device().createPipeline(d);
        }
        rhi::ProgramDesc pg; pg.vs = "geometry/terrain.vert"; pg.fs = "geometry/terrain.frag"; pg.defines = {"TERRAIN_NO_TESS"};
        pg.name = "terrain.grid";
        d.program = e.shaders().get(pg); d.topology = rhi::Topology::Triangles; d.raster.fill = rhi::FillMode::Solid; d.name = "terrain.grid";
        gridPipe_ = e.device().createPipeline(d);
        params_ = geo::makeBuffer(e, sizeof(TerrainParams), rhi::BufUsageUniform, "terrain.params", name());

        // ---- POM floor: procedural brick depth + albedo
        const int N = 512;
        std::vector<uint8_t> depth(N * N), albedo(N * N * 4);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float u = float(x) / N * 4.0f, v = float(y) / N * 8.0f;        // 4 x 8 bricks per tile
                int row = int(v); float bu = u + (row & 1) * 0.5f;
                float fu = bu - std::floor(bu), fv = v - std::floor(v);
                float edge = std::min({fu, 1.0f - fu, fv * 0.5f, (1.0f - fv) * 0.5f});
                float mortar = glm::smoothstep(0.0f, 0.06f, edge);                 // 0 in the gaps
                uint32_t hsh = uint32_t(int(bu) * 73856093) ^ uint32_t(row * 19349663);
                float var = float((hsh >> 8) & 255) / 255.0f;
                float bump = 0.06f * std::sin(x * 0.35f) * std::sin(y * 0.29f);
                float dpt = glm::clamp(1.0f - mortar * (0.9f + bump), 0.0f, 1.0f);
                depth[y * N + x] = uint8_t(dpt * 255.0f);
                glm::vec3 brick = glm::mix(glm::vec3(0.45f, 0.17f, 0.10f), glm::vec3(0.62f, 0.30f, 0.18f), var);
                glm::vec3 c = glm::mix(glm::vec3(0.55f, 0.53f, 0.50f), brick, mortar);
                for (int k = 0; k < 3; ++k) albedo[(y * N + x) * 4 + k] = uint8_t(std::pow(c[k], 1.0f / 2.2f) * 255.0f);
                albedo[(y * N + x) * 4 + 3] = 255;
            }
        rhi::TextureDesc dd; dd.format = rhi::Format::R8; dd.width = dd.height = N; dd.mipLevels = 0; dd.name = "pom.depth"; dd.owner = name();
        pomDepthTex_ = e.device().createTexture(dd, depth.data());
        dd.format = rhi::Format::RGBA8_SRGB; dd.name = "pom.albedo";
        pomAlbedoTex_ = e.device().createTexture(dd, albedo.data());
        e.device().cmd().generateMips(pomDepthTex_.get());
        e.device().cmd().generateMips(pomAlbedoTex_.get());
        geo::SimpleMesh quad = geo::makeGridXZ(1, 12.0f);
        pomVB_ = geo::makeBuffer(e, quad.vertices.size() * sizeof(geo::SimpleVertex), rhi::BufUsageVertex, "pom.vb", name(), quad.vertices.data());
        pomIB_ = geo::makeBuffer(e, quad.indices.size() * 4, rhi::BufUsageIndex, "pom.ib", name(), quad.indices.data());
        pomParams_ = geo::makeBuffer(e, sizeof(PomParams), rhi::BufUsageUniform, "pom.params", name());
        rhi::PipelineDesc pd = geo::gbufferPipelineDesc(e.shaders().graphics("geometry/pom.vert", "geometry/pom.frag"), "pom");
        pomPipe_ = e.device().createPipeline(pd);
    }

    void addPasses(PassList& p) override {
        auto showcase = [](FrameContext& ctx) { return ctx.settings.getBool("world.showcase"); };
        p.add(Stage::GBuffer, "Terrain", [this, showcase](FrameContext& ctx) {
            if (!showcase(ctx) || ctx.msaaForward) return;
            drawTerrain(ctx);
        }, 10.0f);
        p.add(Stage::GBuffer, "POM floor", [this, showcase](FrameContext& ctx) {
            if (!showcase(ctx) || ctx.msaaForward) return;
            drawPom(ctx);
        }, 12.0f);
    }

private:
    // Terrain quality -> patch grid per side
    static int patchGrid(int q) { static const int g[4] = {16, 24, 32, 48}; return g[std::clamp(q, 0, 3)]; }

    void buildPatchBuffers(Engine& e) {
        for (int q = 0; q < 4; ++q) {
            int n = patchGrid(q);
            std::vector<glm::vec3> v;
            float s = geo::kTerrainSize / n;
            for (int z = 0; z < n; ++z)
                for (int x = 0; x < n; ++x) {
                    glm::vec3 o = geo::kTerrainCenter + glm::vec3(-geo::kTerrainSize * 0.5f + x * s, 0, -geo::kTerrainSize * 0.5f + z * s);
                    v.push_back(o); v.push_back(o + glm::vec3(s, 0, 0)); v.push_back(o + glm::vec3(s, 0, s)); v.push_back(o + glm::vec3(0, 0, s));
                }
            patchVB_[q] = geo::makeBuffer(e, v.size() * 12, rhi::BufUsageVertex, "terrain.patches", name(), v.data());
            patchCount_[q] = uint32_t(n * n);
            // grid for the no-tessellation path: 8 quads per patch edge (what "Off" looks like)
            int g = n * 8;
            std::vector<glm::vec3> gv; std::vector<uint32_t> gi;
            for (int z = 0; z <= g; ++z)
                for (int x = 0; x <= g; ++x)
                    gv.push_back(geo::kTerrainCenter + glm::vec3((float(x) / g - 0.5f) * geo::kTerrainSize, 0, (float(z) / g - 0.5f) * geo::kTerrainSize));
            for (int z = 0; z < g; ++z)
                for (int x = 0; x < g; ++x) {
                    uint32_t a = z * (g + 1) + x, b = a + g + 1;
                    gi.insert(gi.end(), {a, b, a + 1, a + 1, b, b + 1});
                }
            gridVB_[q] = geo::makeBuffer(e, gv.size() * 12, rhi::BufUsageVertex, "terrain.grid.vb", name(), gv.data());
            gridIB_[q] = geo::makeBuffer(e, gi.size() * 4, rhi::BufUsageIndex, "terrain.grid.ib", name(), gi.data());
            gridIndexCount_[q] = uint32_t(gi.size());
        }
    }

    void drawTerrain(FrameContext& ctx) {
        static const float kMaxLevel[5] = {1, 8, 16, 32, 64};
        static const float kTargetPx[5] = {0, 24, 16, 10, 6};
        static const float kLayers[4] = {2, 3, 4, 4};
        static const float kTriplanar[4] = {0, 1, 1, 1};
        int t = tessSupported_ ? tess_->asInt() : 0;
        int q = std::clamp(terrainQ_->asInt(), 0, 3);
        // terrain quality also caps the tessellation level
        static const float kQualityCap[4] = {16, 32, 64, 64};
        TerrainParams prm;
        prm.a = {geo::kTerrainSize, disp_->asFloat() * 30.0f, kTargetPx[t], std::min(kMaxLevel[t], kQualityCap[q])};
        prm.b = {geo::projScaleY(ctx.frame), tessCull_->asBool() ? 1.0f : 0.0f, kLayers[q], kTriplanar[q]};
        prm.c = {geo::kTerrainCenter.x, geo::kTerrainCenter.z, showTris_->asBool() ? 1.0f : 0.0f, 0.0f};
        ctx.cmd.updateBuffer(params_.get(), prm);

        geo::beginGBufferPass(ctx, "Terrain");
        ctx.cmd.bindUniformBuffer(3, params_.get());
        ctx.cmd.bindTexture(0, height_.get(), clampLinear_.get());
        if (t > 0) {
            ctx.cmd.bindPipeline(tessPipe_.get());
            ctx.cmd.bindVertexBuffer(0, patchVB_[q].get());
            ctx.cmd.draw(patchCount_[q] * 4);
        } else {
            ctx.cmd.bindPipeline(gridPipe_.get());
            ctx.cmd.bindVertexBuffer(0, gridVB_[q].get());
            ctx.cmd.bindIndexBuffer(gridIB_[q].get(), rhi::IndexType::U32);
            ctx.cmd.drawIndexed(gridIndexCount_[q]);
        }
        ctx.cmd.endRenderPass();
        ctx.profiler.counter("terrain.patches", t > 0 ? patchCount_[q] : 0);
    }

    void drawPom(FrameContext& ctx) {
        static const float kMin[4] = {0, 8, 12, 16}, kMax[4] = {0, 16, 32, 64}, kRefine[4] = {0, 0, 0, 5}, kShadow[4] = {0, 0, 8, 16};
        int m = std::clamp(pom_->asInt(), 0, 3);
        PomParams prm;
        prm.model = glm::translate(glm::mat4(1.0f), geo::kPlaza + glm::vec3(-6.0f, 0.03f, 6.0f));
        prm.T = {1, 0, 0, 0}; prm.B = {0, 0, 1, 0}; prm.N = {0, 1, 0, 0};
        prm.p0 = {m > 0 ? 1.0f : 0.0f, pomDepth_->asFloat(), kMin[m], kMax[m]};
        prm.p1 = {kRefine[m], kShadow[m], 3.0f, 0.0f};
        ctx.cmd.updateBuffer(pomParams_.get(), prm);
        geo::beginGBufferPass(ctx, "POM floor");
        ctx.cmd.bindPipeline(pomPipe_.get());
        ctx.cmd.bindUniformBuffer(3, pomParams_.get());
        ctx.cmd.bindTexture(0, pomDepthTex_.get(), ctx.engine.linearRepeat());
        ctx.cmd.bindTexture(1, pomAlbedoTex_.get(), ctx.engine.linearRepeat());
        ctx.cmd.bindVertexBuffer(0, pomVB_.get());
        ctx.cmd.bindIndexBuffer(pomIB_.get(), rhi::IndexType::U32);
        ctx.cmd.drawIndexed(6);
        ctx.cmd.endRenderPass();
    }

    Setting *tess_ = nullptr, *disp_ = nullptr, *tessCull_ = nullptr, *showTris_ = nullptr, *terrainQ_ = nullptr;
    Setting *pom_ = nullptr, *pomDepth_ = nullptr;
    bool tessSupported_ = true;
    rhi::TexturePtr height_, pomDepthTex_, pomAlbedoTex_;
    rhi::SamplerPtr clampLinear_;
    rhi::BufferPtr patchVB_[4], gridVB_[4], gridIB_[4], params_, pomVB_, pomIB_, pomParams_;
    uint32_t patchCount_[4] = {}, gridIndexCount_[4] = {};
    rhi::PipelinePtr tessPipe_, gridPipe_, pomPipe_;
};

}  // namespace

GFX_REGISTER_MODULE(TerrainModule);
