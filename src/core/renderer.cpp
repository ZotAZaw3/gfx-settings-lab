#include "core/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

#include "core/engine.h"

namespace gfx {

glm::vec3 Camera::forward() const {
    return glm::normalize(glm::vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)));
}
glm::vec3 Camera::right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0))); }
glm::vec3 Camera::up() const { return glm::cross(right(), forward()); }
glm::mat4 Camera::viewMatrix() const { return glm::lookAtRH(position, position + forward(), glm::vec3(0, 1, 0)); }

glm::mat4 Camera::projection(Extent size, glm::vec2 jitter) const {
    float aspect = float(size.width) / float(std::max(1u, size.height));
    float f = 1.0f / std::tan(fovY * 0.5f);
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = f;
    p[2][3] = -1.0f;      // w = -z_view
    p[3][2] = nearPlane;  // z = near  -> depth = near / -z_view (reversed, infinite)
    if (jitter != glm::vec2(0.0f)) {
        // Clip-space translate: shifts the image by `jitter` pixels (+x right, +y down in uv space).
        glm::mat4 t(1.0f);
        t[3][0] = 2.0f * jitter.x / float(size.width);
        t[3][1] = -2.0f * jitter.y / float(size.height);
        p = t * p;
    }
    return p;
}

// ---------------------------------------------------------------------------------------
static constexpr uint32_t kDrawStride = 512;            // sizeof(DrawConstants)=288 rounded to a multiple of 256 (UBO offset alignment)
static constexpr uint32_t kDrawRing = 8192;             // draws per frame before wrapping

rhi::VertexLayout SceneRenderer::vertexLayout() {
    rhi::VertexLayout l;
    l.attribs = {
        {0, rhi::VertexFormat::Float3, offsetof(Vertex, position)},
        {1, rhi::VertexFormat::Float3, offsetof(Vertex, normal)},
        {2, rhi::VertexFormat::Float4, offsetof(Vertex, tangent)},
        {3, rhi::VertexFormat::Float2, offsetof(Vertex, uv)},
    };
    l.strides[0] = sizeof(Vertex);
    return l;
}

static_assert(sizeof(DrawConstants) <= 512);

void SceneRenderer::init(Engine& engine) {
    engine_ = &engine;
    rhi::BufferDesc d;
    d.size = size_t(kDrawStride) * kDrawRing;
    d.usage = rhi::BufUsageUniform;
    d.name = "draw constants ring";
    d.owner = "core.renderer";
    drawUBO_ = engine.device().createBuffer(d);
    staging_.resize(d.size);

    rhi::SamplerDesc s;
    s.maxAnisotropy = 8.0f;
    materialSampler_ = engine.device().createSampler(s);

    auto& sh = engine.shaders();
    auto make = [&](DrawMode mode, bool alphaTest, bool twoSided) {
        rhi::PipelineDesc pd;
        std::vector<std::string> defs;
        if (alphaTest) defs.push_back("ALPHA_TEST=1");
        rhi::ProgramDesc prog;
        prog.vs = "core/scene.vert";
        switch (mode) {
            case DrawMode::DepthOnly: prog.fs = "core/depth.frag"; defs.push_back("DEPTH_ONLY=1"); break;
            case DrawMode::GBuffer:   prog.fs = "core/gbuffer.frag"; break;
            case DrawMode::Forward:   prog.fs = "core/forward.frag"; defs.push_back("FORWARD=1"); break;
            default: break;
        }
        prog.defines = defs;
        pd.program = sh.get(prog);
        pd.layout = vertexLayout();
        pd.raster.cull = twoSided ? rhi::CullMode::None : rhi::CullMode::Back;
        pd.name = prog.fs;
        return engine.device().createPipeline(pd);
    };
    for (int m = 0; m < 3; ++m)
        for (int a = 0; a < 2; ++a)
            for (int t = 0; t < 2; ++t) pipelines_[m][a][t] = make(DrawMode(m), a != 0, t != 0);
    // Transparent forward variant (alpha blend, no depth write)
    for (int t = 0; t < 2; ++t) {
        rhi::PipelineDesc pd = pipelines_[2][0][t]->desc();
        pd.depth.write = false;
        pd.blend[0].mode = rhi::BlendMode::Alpha;
        transparentPipeline_[t] = engine.device().createPipeline(pd);
    }
}

void SceneRenderer::buildDefaultDrawList(FrameContext& ctx) {
    ctx.draws.clear();
    Scene& s = ctx.scene;
    glm::vec3 cam = ctx.camera.position;
    for (uint32_t i = 0; i < s.instances.size(); ++i) {
        const Instance& inst = s.instances[i];
        float dist = glm::length(inst.worldBounds.center() - cam);
        for (uint32_t p : s.meshes[inst.mesh].primitives) {
            DrawItem it{i, p, 0, dist};
            if (s.materials[s.primitives[p].material].alphaMode == AlphaMode::Blend) ctx.draws.transparent.push_back(it);
            else ctx.draws.opaque.push_back(it);
        }
    }
    std::sort(ctx.draws.transparent.begin(), ctx.draws.transparent.end(), [](const DrawItem& a, const DrawItem& b) { return a.distance > b.distance; });
}

void SceneRenderer::beginFrame() { ringHead_ = 0; drawCallsThisFrame = 0; trianglesThisFrame = 0; }

void SceneRenderer::drawScene(FrameContext& ctx, const SceneDrawParams& params) {
    Scene& scene = ctx.scene;
    const DrawList& list = params.list ? *params.list : ctx.draws;
    const auto& items = params.transparent ? list.transparent : list.opaque;
    if (items.empty() || !scene.vertexBuffer) return;
    auto& cmd = ctx.cmd;

    // 1) Write all draw constants for this call into the ring with one upload.
    size_t count = std::min<size_t>(items.size(), kDrawRing);
    if (ringHead_ + count > kDrawRing) ringHead_ = 0;
    uint32_t first = ringHead_;
    for (size_t k = 0; k < count; ++k) {
        const DrawItem& it = items[k];
        const Instance& inst = scene.instances[it.instance];
        const Primitive& prim = scene.primitives[it.primitive];
        const Material& mat = scene.materials[prim.material];
        DrawConstants dc;
        dc.world = inst.world;
        dc.prevWorld = inst.prevWorld;
        dc.viewProj = params.viewProj;
        dc.prevViewProj = params.prevViewProj;
        dc.ids = glm::uvec4(uint32_t(prim.material), it.instance, it.primitive, it.lod);
        dc.params = glm::vec4(params.mipBias, mat.alphaMode == AlphaMode::Mask ? 1.0f : 0.0f, 0, 0);
        std::memcpy(staging_.data() + size_t(first + k) * kDrawStride, &dc, sizeof(dc));
    }
    cmd.updateBuffer(drawUBO_.get(), staging_.data() + size_t(first) * kDrawStride, count * kDrawStride, size_t(first) * kDrawStride);
    ringHead_ += uint32_t(count);

    cmd.bindStorageBuffer(0, scene.lightBuffer.get());
    cmd.bindStorageBuffer(1, scene.materialBuffer.get());

    rhi::Pipeline* bound = nullptr;
    int boundMaterial = -1;
    int modeIdx = params.mode == DrawMode::DepthOnly ? 0 : params.mode == DrawMode::GBuffer ? 1 : 2;
    for (size_t k = 0; k < count; ++k) {
        const DrawItem& it = items[k];
        const Instance& inst = scene.instances[it.instance];
        if (params.shadowCastersOnly && !inst.castShadows) continue;
        const Primitive& prim = scene.primitives[it.primitive];
        const Material& mat = scene.materials[prim.material];
        bool masked = mat.alphaMode == AlphaMode::Mask;
        rhi::Pipeline* pipe = nullptr;
        if (params.pipeline) pipe = (masked && params.maskedPipeline) ? params.maskedPipeline : params.pipeline;
        else if (params.transparent && params.mode == DrawMode::Forward) pipe = transparentPipeline_[mat.doubleSided ? 1 : 0].get();
        else pipe = pipelines_[modeIdx][masked ? 1 : 0][mat.doubleSided ? 1 : 0].get();
        if (pipe != bound) {
            cmd.bindPipeline(pipe);
            cmd.bindVertexBuffer(0, scene.vertexBuffer.get());
            cmd.bindIndexBuffer(scene.indexBuffer.get(), rhi::IndexType::U32);
            bound = pipe;
            boundMaterial = -1;
        }
        if (prim.material != boundMaterial) {
            auto tex = [&](int idx, rhi::Texture* fallback) -> rhi::Texture* {
                return (idx >= 0 && idx < int(scene.textures.size()) && scene.textures[idx].gpu) ? scene.textures[idx].gpu.get() : fallback;
            };
            rhi::Sampler* s = materialSampler_.get();
            cmd.bindTexture(0, tex(mat.baseColorTex, scene.whiteTex.get()), s);
            if (params.mode != DrawMode::DepthOnly || params.pipeline) {
                cmd.bindTexture(1, tex(mat.normalTex, scene.flatNormalTex.get()), s);
                cmd.bindTexture(2, tex(mat.metalRoughTex, scene.whiteTex.get()), s);
                cmd.bindTexture(3, tex(mat.occlusionTex, scene.whiteTex.get()), s);
                cmd.bindTexture(4, tex(mat.emissiveTex, scene.whiteTex.get()), s);
                cmd.bindTexture(5, tex(mat.heightTex, scene.blackTex.get()), s);
            }
            boundMaterial = prim.material;
        }
        cmd.bindUniformBuffer(1, drawUBO_.get(), size_t(first + k) * kDrawStride, sizeof(DrawConstants));
        uint32_t lod = std::min<uint32_t>(it.lod, uint32_t(prim.lods.size() - 1));
        const LodLevel& L = prim.lods[lod];
        if (bound->desc().topology == rhi::Topology::Patches)
            cmd.drawIndexed(L.indexCount, 1, L.firstIndex, int32_t(prim.baseVertex));
        else
            cmd.drawIndexed(L.indexCount, 1, L.firstIndex, int32_t(prim.baseVertex));
        ++drawCallsThisFrame;
        trianglesThisFrame += L.indexCount / 3;
    }
}

}  // namespace gfx
