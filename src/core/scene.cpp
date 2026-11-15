#include "core/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace gfx {

AABB AABB::transformed(const glm::mat4& m) const {
    AABB r;
    if (!valid()) return r;
    for (int i = 0; i < 8; ++i) {
        glm::vec3 p((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z);
        r.expand(glm::vec3(m * glm::vec4(p, 1.0f)));
    }
    return r;
}

// ---------------------------------------------------------------------------------------
// CPU mip chain generation (box filter; sRGB textures are filtered in linear space)
// ---------------------------------------------------------------------------------------
static float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
static float linearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

static void buildMips(SceneTexture& t) {
    static float toLin[256];
    static bool init = false;
    if (!init) { for (int i = 0; i < 256; ++i) toLin[i] = srgbToLinear(i / 255.0f); init = true; }
    uint32_t w = t.width, h = t.height;
    t.mipCount = 1;
    while (w > 1 || h > 1) {
        uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const auto& src = t.cpuMips.back();
        std::vector<uint8_t> dst(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x) {
                uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                for (int c = 0; c < 4; ++c) {
                    auto px = [&](uint32_t xx, uint32_t yy) { return src[(size_t(yy) * w + xx) * 4 + c]; };
                    float v;
                    if (t.srgb && c < 3)
                        v = linearToSrgb((toLin[px(x0, y0)] + toLin[px(x1, y0)] + toLin[px(x0, y1)] + toLin[px(x1, y1)]) * 0.25f);
                    else
                        v = (px(x0, y0) + px(x1, y0) + px(x0, y1) + px(x1, y1)) * 0.25f / 255.0f;
                    dst[(size_t(y) * nw + x) * 4 + c] = uint8_t(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            }
        t.cpuMips.push_back(std::move(dst));
        w = nw; h = nh;
        ++t.mipCount;
    }
}

static int addTexture(Scene& s, std::string name, uint32_t w, uint32_t h, std::vector<uint8_t> rgba, bool srgb) {
    SceneTexture t;
    t.name = std::move(name);
    t.width = w; t.height = h; t.srgb = srgb;
    t.cpuMips.push_back(std::move(rgba));
    buildMips(t);
    s.textures.push_back(std::move(t));
    return int(s.textures.size()) - 1;
}

// ---------------------------------------------------------------------------------------
// Tangent generation (per-vertex accumulation; good enough for a prototype; MikkTSpace is the reference)
// ---------------------------------------------------------------------------------------
static void generateTangents(std::vector<Vertex>& v, const std::vector<uint32_t>& idx, uint32_t base, uint32_t count) {
    std::vector<glm::vec3> tan(count, glm::vec3(0)), bit(count, glm::vec3(0));
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a >= count || b >= count || c >= count) continue;
        Vertex &va = v[base + a], &vb = v[base + b], &vc = v[base + c];
        glm::vec3 e1 = vb.position - va.position, e2 = vc.position - va.position;
        glm::vec2 d1 = vb.uv - va.uv, d2 = vc.uv - va.uv;
        float r = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(r) < 1e-12f) continue;
        r = 1.0f / r;
        glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r, bt = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t k : {a, b, c}) { tan[k] += t; bit[k] += bt; }
    }
    for (uint32_t i = 0; i < count; ++i) {
        glm::vec3 n = v[base + i].normal, t = tan[i];
        t = t - n * glm::dot(n, t);
        if (glm::dot(t, t) < 1e-12f) t = std::abs(n.x) < 0.9f ? glm::cross(n, glm::vec3(1, 0, 0)) : glm::cross(n, glm::vec3(0, 1, 0));
        t = glm::normalize(t);
        float w = glm::dot(glm::cross(n, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
        v[base + i].tangent = glm::vec4(t, w);
    }
}

uint32_t Scene::addLod(uint32_t primitive, const std::vector<uint32_t>& lodIndices, float error) {
    Primitive& p = primitives[primitive];
    LodLevel l;
    l.firstIndex = uint32_t(indices.size());
    l.indexCount = uint32_t(lodIndices.size());
    l.error = error;
    indices.insert(indices.end(), lodIndices.begin(), lodIndices.end());
    p.lods.push_back(l);
    markGeometryDirty();
    return uint32_t(p.lods.size() - 1);
}

// ---------------------------------------------------------------------------------------
// glTF 2.0 loader (cgltf)
// ---------------------------------------------------------------------------------------
bool Scene::loadGltf(const std::string& path, rhi::Device& device, std::string* error) {
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) { if (error) *error = "cannot parse " + path; return false; }
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) { if (error) *error = "cannot load buffers"; cgltf_free(data); return false; }

    *this = Scene();
    sourcePath = path;
    std::filesystem::path dir = std::filesystem::path(path).parent_path();

    // Images -> textures. glTF stores color in sRGB and data maps (normal, MR, AO) in linear;
    // the same image can be used both ways, so key by (image, srgb).
    std::map<std::pair<const cgltf_image*, bool>, int> imageTex;
    auto textureFor = [&](const cgltf_texture_view& view, bool srgb) -> int {
        if (!view.texture || !view.texture->image) return -1;
        const cgltf_image* img = view.texture->image;
        auto key = std::make_pair(img, srgb);
        if (auto it = imageTex.find(key); it != imageTex.end()) return it->second;
        int w = 0, h = 0, n = 0;
        stbi_uc* pixels = nullptr;
        if (img->buffer_view) {
            const uint8_t* bytes = cgltf_buffer_view_data(img->buffer_view);
            pixels = stbi_load_from_memory(bytes, int(img->buffer_view->size), &w, &h, &n, 4);
        } else if (img->uri) {
            std::string uri = img->uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            pixels = stbi_load((dir / uri).string().c_str(), &w, &h, &n, 4);
        }
        if (!pixels) { imageTex[key] = -1; return -1; }
        std::vector<uint8_t> rgba(pixels, pixels + size_t(w) * h * 4);
        stbi_image_free(pixels);
        int idx = addTexture(*this, img->name ? img->name : (img->uri ? img->uri : "embedded"), uint32_t(w), uint32_t(h), std::move(rgba), srgb);
        imageTex[key] = idx;
        return idx;
    };

    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        Material mat;
        mat.name = m.name ? m.name : "material";
        if (m.has_pbr_metallic_roughness) {
            const auto& pbr = m.pbr_metallic_roughness;
            mat.baseColor = glm::make_vec4(pbr.base_color_factor);
            mat.metallic = pbr.metallic_factor;
            mat.roughness = pbr.roughness_factor;
            mat.baseColorTex = textureFor(pbr.base_color_texture, true);
            mat.metalRoughTex = textureFor(pbr.metallic_roughness_texture, false);
        }
        mat.normalTex = textureFor(m.normal_texture, false);
        mat.normalScale = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
        mat.occlusionTex = textureFor(m.occlusion_texture, false);
        mat.occlusionStrength = m.occlusion_texture.texture ? m.occlusion_texture.scale : 1.0f;
        mat.emissiveTex = textureFor(m.emissive_texture, true);
        mat.emissive = glm::make_vec3(m.emissive_factor);
        if (m.has_emissive_strength) mat.emissive *= m.emissive_strength.emissive_strength;
        mat.alphaMode = m.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask : m.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend : AlphaMode::Opaque;
        mat.alphaCutoff = m.alpha_cutoff;
        mat.doubleSided = m.double_sided;
        if (m.has_volume || m.has_transmission) mat.shadingModel = ShadingModel::Subsurface;
        if (m.has_clearcoat) mat.shadingModel = ShadingModel::ClearCoat;
        if (m.unlit) mat.shadingModel = ShadingModel::Unlit;
        materials.push_back(mat);
    }
    if (materials.empty()) materials.push_back(Material{});

    // Meshes -> primitives
    std::map<const cgltf_mesh*, uint32_t> meshIndex;
    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& gm = data->meshes[mi];
        Mesh mesh;
        mesh.name = gm.name ? gm.name : "mesh";
        for (size_t pi = 0; pi < gm.primitives_count; ++pi) {
            const cgltf_primitive& gp = gm.primitives[pi];
            if (gp.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *tan = nullptr, *uv = nullptr;
            for (size_t a = 0; a < gp.attributes_count; ++a) {
                const auto& at = gp.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_tangent) tan = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
            }
            if (!pos) continue;
            Primitive prim;
            prim.baseVertex = uint32_t(vertices.size());
            prim.vertexCount = uint32_t(pos->count);
            prim.material = gp.material ? int(gp.material - data->materials) : 0;
            for (size_t v = 0; v < pos->count; ++v) {
                Vertex vx{};
                cgltf_accessor_read_float(pos, v, glm::value_ptr(vx.position), 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, glm::value_ptr(vx.normal), 3); else vx.normal = glm::vec3(0, 1, 0);
                if (tan) cgltf_accessor_read_float(tan, v, glm::value_ptr(vx.tangent), 4);
                if (uv) cgltf_accessor_read_float(uv, v, glm::value_ptr(vx.uv), 2);
                prim.bounds.expand(vx.position);
                vertices.push_back(vx);
            }
            std::vector<uint32_t> local;
            if (gp.indices) {
                local.resize(gp.indices->count);
                for (size_t k = 0; k < gp.indices->count; ++k) local[k] = uint32_t(cgltf_accessor_read_index(gp.indices, k));
            } else {
                local.resize(pos->count);
                for (size_t k = 0; k < pos->count; ++k) local[k] = uint32_t(k);
            }
            if (!nrm) {   // flat-ish normals from faces
                for (uint32_t k = 0; k < prim.vertexCount; ++k) vertices[prim.baseVertex + k].normal = glm::vec3(0);
                for (size_t k = 0; k + 2 < local.size(); k += 3) {
                    auto& a = vertices[prim.baseVertex + local[k]]; auto& b = vertices[prim.baseVertex + local[k + 1]]; auto& c = vertices[prim.baseVertex + local[k + 2]];
                    glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
                    a.normal += n; b.normal += n; c.normal += n;
                }
                for (uint32_t k = 0; k < prim.vertexCount; ++k) {
                    auto& n = vertices[prim.baseVertex + k].normal;
                    n = glm::dot(n, n) > 0 ? glm::normalize(n) : glm::vec3(0, 1, 0);
                }
            }
            if (!tan) generateTangents(vertices, local, prim.baseVertex, prim.vertexCount);
            LodLevel l0;
            l0.firstIndex = uint32_t(indices.size());
            l0.indexCount = uint32_t(local.size());
            indices.insert(indices.end(), local.begin(), local.end());
            prim.lods.push_back(l0);
            mesh.primitives.push_back(uint32_t(primitives.size()));
            primitives.push_back(std::move(prim));
        }
        meshIndex[&gm] = uint32_t(meshes.size());
        meshes.push_back(std::move(mesh));
    }

    // Nodes -> instances (world transforms)
    std::function<void(const cgltf_node*, const glm::mat4&)> visit = [&](const cgltf_node* n, const glm::mat4& parent) {
        float local[16];
        cgltf_node_transform_local(n, local);
        glm::mat4 world = parent * glm::make_mat4(local);
        if (n->mesh) {
            Instance inst;
            inst.world = inst.prevWorld = world;
            inst.mesh = meshIndex[n->mesh];
            instances.push_back(inst);
        }
        if (n->light) {
            Light L;
            L.type = n->light->type == cgltf_light_type_spot ? LightType::Spot : LightType::Point;
            L.position = glm::vec3(world[3]);
            L.direction = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
            L.color = glm::make_vec3(n->light->color);
            L.intensity = n->light->intensity * 0.01f;
            L.range = n->light->range > 0 ? n->light->range : 15.0f;
            L.innerCone = n->light->spot_inner_cone_angle; L.outerCone = n->light->spot_outer_cone_angle;
            if (n->light->type != cgltf_light_type_directional) lights.push_back(L);
            else sun.direction = -L.direction;
        }
        for (size_t c = 0; c < n->children_count; ++c) visit(n->children[c], world);
    };
    const cgltf_scene* sc = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (sc) for (size_t i = 0; i < sc->nodes_count; ++i) visit(sc->nodes[i], glm::mat4(1.0f));
    else for (size_t i = 0; i < data->nodes_count; ++i) if (!data->nodes[i].parent) visit(&data->nodes[i], glm::mat4(1.0f));

    cgltf_free(data);
    updateInstances();
    for (auto& inst : instances) inst.prevWorld = inst.world;
    markGeometryDirty();
    markMaterialsDirty();
    upload(device);
    std::printf("[scene] %s: %zu meshes, %zu primitives, %zu instances, %zu vertices, %zu triangles, %zu textures\n",
                path.c_str(), meshes.size(), primitives.size(), instances.size(), vertices.size(), indices.size() / 3, textures.size());
    return true;
}

// ---------------------------------------------------------------------------------------
// Procedural demo scene
// ---------------------------------------------------------------------------------------
namespace {
struct MeshBuilder {
    std::vector<Vertex> v;
    std::vector<uint32_t> i;
    void quad(glm::vec3 o, glm::vec3 u, glm::vec3 w, glm::vec2 uvScale) {
        uint32_t b = uint32_t(v.size());
        glm::vec3 n = glm::normalize(glm::cross(u, w));   // CCW (o, o+u, o+u+w) seen from the normal side
        glm::vec3 c[4] = {o, o + u, o + u + w, o + w};
        glm::vec2 t[4] = {{0, 0}, {uvScale.x, 0}, {uvScale.x, uvScale.y}, {0, uvScale.y}};
        for (int k = 0; k < 4; ++k) v.push_back({c[k], n, glm::vec4(glm::normalize(u), 1.0f), t[k]});
        i.insert(i.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
};

MeshBuilder makePlane(float size, int segments, float uvRepeat) {
    MeshBuilder m;
    for (int z = 0; z <= segments; ++z)
        for (int x = 0; x <= segments; ++x) {
            float fx = float(x) / segments, fz = float(z) / segments;
            m.v.push_back({{(fx - 0.5f) * size, 0.0f, (fz - 0.5f) * size}, {0, 1, 0}, {1, 0, 0, 1}, {fx * uvRepeat, fz * uvRepeat}});
        }
    for (int z = 0; z < segments; ++z)
        for (int x = 0; x < segments; ++x) {
            uint32_t a = z * (segments + 1) + x, b = a + 1, c = a + segments + 1, d = c + 1;
            m.i.insert(m.i.end(), {a, c, b, b, c, d});
        }
    return m;
}

MeshBuilder makeSphere(float r, int seg, int rings) {
    MeshBuilder m;
    for (int y = 0; y <= rings; ++y)
        for (int x = 0; x <= seg; ++x) {
            float u = float(x) / seg, vv = float(y) / rings;
            float th = u * 6.2831853f, ph = vv * 3.14159265f;
            glm::vec3 n(std::sin(ph) * std::cos(th), std::cos(ph), std::sin(ph) * std::sin(th));
            glm::vec3 t(-std::sin(th), 0, std::cos(th));
            m.v.push_back({n * r, n, glm::vec4(t, 1.0f), {u * 2.0f, vv}});
        }
    for (int y = 0; y < rings; ++y)
        for (int x = 0; x < seg; ++x) {
            uint32_t a = y * (seg + 1) + x, b = a + 1, c = a + seg + 1, d = c + 1;
            m.i.insert(m.i.end(), {a, b, c, b, d, c});
        }
    return m;
}

MeshBuilder makeBox(glm::vec3 h) {
    MeshBuilder m;
    m.quad({-h.x, -h.y, h.z}, {2 * h.x, 0, 0}, {0, 2 * h.y, 0}, {1, 1});    // +Z
    m.quad({h.x, -h.y, -h.z}, {-2 * h.x, 0, 0}, {0, 2 * h.y, 0}, {1, 1});   // -Z
    m.quad({h.x, -h.y, h.z}, {0, 0, -2 * h.z}, {0, 2 * h.y, 0}, {1, 1});    // +X
    m.quad({-h.x, -h.y, -h.z}, {0, 0, 2 * h.z}, {0, 2 * h.y, 0}, {1, 1});   // -X
    m.quad({-h.x, h.y, h.z}, {2 * h.x, 0, 0}, {0, 0, -2 * h.z}, {1, 1});    // +Y
    m.quad({-h.x, -h.y, -h.z}, {2 * h.x, 0, 0}, {0, 0, 2 * h.z}, {1, 1});   // -Y
    return m;
}

float hash2(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h & 0xFFFFFF) / float(0xFFFFFF);
}
}  // namespace

void Scene::buildProceduralDemo(rhi::Device& device) {
    *this = Scene();
    sourcePath = "procedural";

    // --- textures: brick albedo / normal / height / roughness, checker, leaf alpha ---
    const uint32_t N = 1024;
    std::vector<uint8_t> albedo(N * N * 4), normal(N * N * 4), height(N * N * 4), mr(N * N * 4), checker(N * N * 4);
    std::vector<float> hgt(N * N);
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const float bw = 128.0f, bh = 64.0f, mortar = 5.0f;
            int row = int(y / bh);
            float ox = (row & 1) ? bw * 0.5f : 0.0f;
            float bx = std::fmod(x + ox, bw), by = std::fmod(float(y), bh);
            float edge = std::min({bx, bw - bx, by, bh - by});
            float brick = std::clamp((edge - mortar) / 4.0f, 0.0f, 1.0f);
            int bxi = int((x + ox) / bw);
            float tint = hash2(bxi, row);
            float noise = hash2(int(x), int(y)) * 0.15f;
            hgt[y * N + x] = brick * (0.85f + noise);
            glm::vec3 c = glm::mix(glm::vec3(0.55f, 0.53f, 0.5f), glm::vec3(0.55f + 0.2f * tint, 0.22f + 0.08f * tint, 0.14f), brick) * (0.9f + noise);
            size_t o = (size_t(y) * N + x) * 4;
            albedo[o] = uint8_t(std::min(255.0f, c.r * 255)); albedo[o + 1] = uint8_t(std::min(255.0f, c.g * 255)); albedo[o + 2] = uint8_t(std::min(255.0f, c.b * 255)); albedo[o + 3] = 255;
            mr[o] = 255; mr[o + 1] = uint8_t((0.55f + 0.4f * (1.0f - brick) + noise) * 200); mr[o + 2] = 0; mr[o + 3] = 255;
            bool chk = ((x / 64) + (y / 64)) & 1;
            uint8_t cv = chk ? 230 : 40;
            checker[o] = cv; checker[o + 1] = cv; checker[o + 2] = cv; checker[o + 3] = 255;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            float hl = hgt[y * N + (x + N - 1) % N], hr = hgt[y * N + (x + 1) % N];
            float hu = hgt[((y + N - 1) % N) * N + x], hd = hgt[((y + 1) % N) * N + x];
            glm::vec3 n = glm::normalize(glm::vec3((hl - hr) * 3.0f, (hu - hd) * 3.0f, 1.0f));
            size_t o = (size_t(y) * N + x) * 4;
            normal[o] = uint8_t((n.x * 0.5f + 0.5f) * 255); normal[o + 1] = uint8_t((n.y * 0.5f + 0.5f) * 255); normal[o + 2] = uint8_t((n.z * 0.5f + 0.5f) * 255); normal[o + 3] = 255;
            uint8_t hv = uint8_t(hgt[y * N + x] * 255);
            height[o] = hv; height[o + 1] = hv; height[o + 2] = hv; height[o + 3] = 255;
        }
    int tBrickA = addTexture(*this, "brick_albedo", N, N, albedo, true);
    int tBrickN = addTexture(*this, "brick_normal", N, N, normal, false);
    int tBrickH = addTexture(*this, "brick_height", N, N, height, false);
    int tBrickMR = addTexture(*this, "brick_mr", N, N, mr, false);
    int tChecker = addTexture(*this, "checker", N, N, checker, true);

    const uint32_t L = 256;
    std::vector<uint8_t> leaf(L * L * 4);
    for (uint32_t y = 0; y < L; ++y)
        for (uint32_t x = 0; x < L; ++x) {
            float u = (x + 0.5f) / L * 2 - 1, v = (y + 0.5f) / L;
            float width = std::sin(v * 3.14159f) * 0.8f;
            bool inside = std::abs(u) < width && v > 0.02f;
            bool vein = std::abs(u) < 0.02f;
            size_t o = (size_t(y) * L + x) * 4;
            leaf[o] = uint8_t(vein ? 120 : 60); leaf[o + 1] = uint8_t(vein ? 160 : 130 + 40 * v); leaf[o + 2] = 40; leaf[o + 3] = inside ? 255 : 0;
        }
    int tLeaf = addTexture(*this, "leaf", L, L, leaf, true);

    // --- materials ---
    auto addMat = [&](Material m) { materials.push_back(m); return int(materials.size()) - 1; };
    Material ground; ground.name = "ground_brick"; ground.baseColorTex = tBrickA; ground.normalTex = tBrickN; ground.metalRoughTex = tBrickMR;
    ground.heightTex = tBrickH; ground.displacementScale = 0.06f; ground.roughness = 1.0f;
    int mGround = addMat(ground);
    Material chk; chk.name = "checker_filtering"; chk.baseColorTex = tChecker; chk.roughness = 0.6f;
    int mChecker = addMat(chk);
    Material leafM; leafM.name = "foliage"; leafM.baseColorTex = tLeaf; leafM.alphaMode = AlphaMode::Mask; leafM.doubleSided = true;
    leafM.shadingModel = ShadingModel::Foliage; leafM.roughness = 0.7f;
    int mLeaf = addMat(leafM);
    Material skin; skin.name = "subsurface_skin"; skin.baseColor = glm::vec4(0.9f, 0.62f, 0.5f, 1); skin.roughness = 0.45f;
    skin.shadingModel = ShadingModel::Subsurface; skin.subsurfaceRadius = 3.0f; skin.subsurfaceColor = glm::vec3(0.9f, 0.3f, 0.2f);
    int mSkin = addMat(skin);
    Material emis; emis.name = "emissive"; emis.baseColor = glm::vec4(0.05f, 0.05f, 0.05f, 1); emis.emissive = glm::vec3(8.0f, 3.5f, 1.2f);
    int mEmissive = addMat(emis);
    Material glass; glass.name = "glass_blend"; glass.baseColor = glm::vec4(0.6f, 0.8f, 1.0f, 0.35f); glass.roughness = 0.05f; glass.alphaMode = AlphaMode::Blend;
    int mGlass = addMat(glass);
    Material mirror; mirror.name = "mirror"; mirror.baseColor = glm::vec4(0.95f); mirror.metallic = 1.0f; mirror.roughness = 0.03f;
    int mMirror = addMat(mirror);
    Material wall; wall.name = "wall"; wall.baseColor = glm::vec4(0.8f, 0.78f, 0.74f, 1); wall.roughness = 0.85f;
    int mWall = addMat(wall);
    std::vector<int> sphereMats;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 5; ++c) {
            Material m; m.name = "sphere";
            m.metallic = float(r);
            m.roughness = 0.05f + 0.95f * c / 4.0f;
            m.baseColor = r ? glm::vec4(1.0f, 0.78f, 0.34f, 1) : glm::vec4(0.8f, 0.1f, 0.1f, 1);
            sphereMats.push_back(addMat(m));
        }

    // --- meshes ---
    auto addMesh = [&](const std::string& name, const MeshBuilder& b, int material) {
        Primitive p;
        p.baseVertex = uint32_t(vertices.size());
        p.vertexCount = uint32_t(b.v.size());
        p.material = material;
        for (auto& v : b.v) { vertices.push_back(v); p.bounds.expand(v.position); }
        LodLevel l; l.firstIndex = uint32_t(indices.size()); l.indexCount = uint32_t(b.i.size());
        indices.insert(indices.end(), b.i.begin(), b.i.end());
        p.lods.push_back(l);
        Mesh m; m.name = name; m.primitives.push_back(uint32_t(primitives.size()));
        primitives.push_back(p);
        meshes.push_back(m);
        return uint32_t(meshes.size() - 1);
    };
    auto place = [&](uint32_t mesh, glm::mat4 world, bool shadows = true) {
        Instance i; i.mesh = mesh; i.world = i.prevWorld = world; i.castShadows = shadows; instances.push_back(i);
    };

    uint32_t plane = addMesh("ground", makePlane(40.0f, 64, 12.0f), mGround);
    place(plane, glm::mat4(1.0f), false);
    uint32_t chkPlane = addMesh("filtering_strip", makePlane(1.0f, 1, 60.0f), mChecker);
    place(chkPlane, glm::translate(glm::mat4(1), {-12.0f, 0.01f, -5.0f}) * glm::scale(glm::mat4(1), {3.0f, 1.0f, 30.0f}), false);

    std::vector<uint32_t> sphereMeshes;
    for (size_t k = 0; k < sphereMats.size(); ++k) {
        uint32_t m = addMesh("sphere", makeSphere(0.5f, 48, 32), sphereMats[k]);
        int r = int(k / 5), c = int(k % 5);
        place(m, glm::translate(glm::mat4(1), {-3.0f + c * 1.5f, 0.6f + r * 1.3f, -2.0f}));
    }
    uint32_t sss = addMesh("sss_sphere", makeSphere(0.8f, 64, 48), mSkin);
    place(sss, glm::translate(glm::mat4(1), {5.0f, 0.8f, 1.0f}));
    uint32_t emissive = addMesh("emissive_sphere", makeSphere(0.3f, 32, 16), mEmissive);
    place(emissive, glm::translate(glm::mat4(1), {3.0f, 1.8f, 2.5f}), false);
    uint32_t mirrorBox = addMesh("mirror", makeBox({0.05f, 1.2f, 2.0f}), mMirror);
    place(mirrorBox, glm::translate(glm::mat4(1), {7.5f, 1.2f, -3.0f}) * glm::rotate(glm::mat4(1), -0.5f, glm::vec3(0, 1, 0)));
    uint32_t glassBox = addMesh("glass", makeBox({0.6f, 0.6f, 0.6f}), mGlass);
    place(glassBox, glm::translate(glm::mat4(1), {1.5f, 0.6f, 3.0f}));
    // Pillars + wall: shadows, AO, god rays through gaps
    uint32_t pillar = addMesh("pillar", makeBox({0.3f, 2.5f, 0.3f}), mWall);
    for (int k = 0; k < 6; ++k) place(pillar, glm::translate(glm::mat4(1), {-6.0f + k * 2.4f, 2.5f, -6.0f}));
    uint32_t beam = addMesh("beam", makeBox({7.0f, 0.3f, 0.6f}), mWall);
    place(beam, glm::translate(glm::mat4(1), {0.0f, 5.3f, -6.0f}));
    uint32_t wallM = addMesh("wall", makeBox({8.0f, 3.0f, 0.2f}), mWall);
    place(wallM, glm::translate(glm::mat4(1), {0.0f, 3.0f, -9.0f}));
    uint32_t cube = addMesh("cube", makeBox({0.5f, 0.5f, 0.5f}), mWall);
    place(cube, glm::translate(glm::mat4(1), {-1.0f, 0.5f, 1.5f}) * glm::rotate(glm::mat4(1), 0.6f, glm::vec3(0, 1, 0)));
    // Foliage: crossed alpha-tested cards
    MeshBuilder card;
    for (int k = 0; k < 3; ++k) {
        float a = k * 1.0472f;
        glm::vec3 u(std::cos(a) * 1.0f, 0, std::sin(a) * 1.0f);
        card.quad(-u * 0.5f, u, {0, 1.4f, 0}, {1, 1});
    }
    for (auto& v : card.v) v.uv.y = 1.0f - v.uv.y;
    uint32_t bush = addMesh("bush", card, mLeaf);
    for (int k = 0; k < 12; ++k)
        place(bush, glm::translate(glm::mat4(1), {-9.0f + hash2(k, 1) * 4.0f, 0.0f, 2.0f + hash2(k, 7) * 4.0f}) *
                        glm::rotate(glm::mat4(1), hash2(k, 3) * 6.28f, glm::vec3(0, 1, 0)));

    // --- lights ---
    Light l1; l1.position = {3.0f, 1.8f, 2.5f}; l1.color = {1.0f, 0.55f, 0.25f}; l1.intensity = 6.0f; l1.range = 8.0f; lights.push_back(l1);
    Light l2; l2.position = {-4.0f, 2.0f, 2.0f}; l2.color = {0.3f, 0.5f, 1.0f}; l2.intensity = 5.0f; l2.range = 9.0f; lights.push_back(l2);
    Light s1; s1.type = LightType::Spot; s1.position = {6.0f, 4.0f, 3.0f}; s1.direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f));
    s1.color = {1.0f, 0.95f, 0.8f}; s1.intensity = 20.0f; s1.range = 12.0f; s1.castShadows = true; lights.push_back(s1);

    updateInstances();
    markGeometryDirty();
    markMaterialsDirty();
    upload(device);
}

// ---------------------------------------------------------------------------------------
void Scene::updateInstances() {
    bounds = AABB();
    for (auto& inst : instances) {
        AABB local;
        for (uint32_t p : meshes[inst.mesh].primitives) local.expand(primitives[p].bounds);
        inst.worldBounds = local.transformed(inst.world);
        bounds.expand(inst.worldBounds);
    }
}

void Scene::upload(rhi::Device& device) {
    if (!whiteTex) {
        uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255}, black[4] = {0, 0, 0, 255};
        rhi::TextureDesc d; d.format = rhi::Format::RGBA8; d.name = "white"; d.owner = "scene";
        whiteTex = device.createTexture(d, white);
        d.name = "flatNormal"; flatNormalTex = device.createTexture(d, flat);
        d.name = "black"; blackTex = device.createTexture(d, black);
    }
    if (geometryDirty_) {
        rhi::BufferDesc vb; vb.size = std::max<size_t>(16, vertices.size() * sizeof(Vertex));
        vb.usage = rhi::BufUsageVertex | rhi::BufUsageStorage; vb.dynamic = false; vb.structStride = sizeof(Vertex);
        vb.name = "scene.vertices"; vb.owner = "scene";
        vertexBuffer = device.createBuffer(vb, vertices.empty() ? nullptr : vertices.data());
        rhi::BufferDesc ib; ib.size = std::max<size_t>(16, indices.size() * 4);
        ib.usage = rhi::BufUsageIndex | rhi::BufUsageStorage; ib.dynamic = false; ib.structStride = 4;
        ib.name = "scene.indices"; ib.owner = "scene";
        indexBuffer = device.createBuffer(ib, indices.empty() ? nullptr : indices.data());
        geometryDirty_ = false;
    }
    if (materialsDirty_) {
        std::vector<GpuMaterial> gm;
        for (auto& m : materials) {
            GpuMaterial g;
            g.baseColor = m.baseColor;
            g.emissiveRough = glm::vec4(m.emissive, m.roughness);
            g.params = glm::vec4(m.metallic, m.normalScale, m.occlusionStrength, m.alphaCutoff);
            g.sss = glm::vec4(m.subsurfaceColor, m.subsurfaceRadius);
            g.tex0 = glm::ivec4(m.baseColorTex, m.normalTex, m.metalRoughTex, m.occlusionTex);
            g.tex1 = glm::ivec4(m.emissiveTex, m.heightTex, int(m.alphaMode), int(m.shadingModel));
            g.params2 = glm::vec4(m.displacementScale, m.doubleSided ? 1.0f : 0.0f, 0, 0);
            gm.push_back(g);
        }
        rhi::BufferDesc mb; mb.size = std::max<size_t>(sizeof(GpuMaterial), gm.size() * sizeof(GpuMaterial));
        mb.usage = rhi::BufUsageStorage; mb.structStride = sizeof(GpuMaterial); mb.name = "scene.materials"; mb.owner = "scene";
        materialBuffer = device.createBuffer(mb, gm.data());
        materialsDirty_ = false;
    }
    // Lights every call (cheap; lights may animate)
    std::vector<GpuLight> gl;
    for (auto& l : lights) {
        GpuLight g;
        g.positionRange = glm::vec4(l.position, l.range);
        g.colorIntensity = glm::vec4(l.color, l.intensity);
        g.directionType = glm::vec4(glm::normalize(l.direction), float(int(l.type)));
        g.cone = glm::vec4(std::cos(l.innerCone), std::cos(l.outerCone), l.castShadows ? 1.0f : 0.0f, -1.0f);
        gl.push_back(g);
    }
    if (gl.empty()) gl.push_back(GpuLight{});
    size_t need = gl.size() * sizeof(GpuLight);
    if (!lightBuffer || lightBuffer->size() < need) {
        rhi::BufferDesc lb; lb.size = std::max<size_t>(need, 64 * sizeof(GpuLight)); lb.usage = rhi::BufUsageStorage;
        lb.structStride = sizeof(GpuLight); lb.name = "scene.lights"; lb.owner = "scene";
        lightBuffer = device.createBuffer(lb);
    }
    device.cmd().updateBuffer(lightBuffer.get(), gl.data(), need, 0);

    // Textures without a streaming system: upload the full chain once.
    for (auto& t : textures) {
        if (t.gpu || t.cpuMips.empty()) continue;
        rhi::TextureDesc d;
        d.format = t.srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8;
        d.width = t.width; d.height = t.height; d.mipLevels = t.mipCount; d.name = t.name; d.owner = "textures";
        t.gpu = device.createTexture(d);
        for (uint32_t m = 0; m < t.mipCount; ++m) device.uploadTexture(t.gpu.get(), m, 0, t.cpuMips[m].data(), t.cpuMips[m].size());
        t.residentTopMip = 0;
    }
}

size_t Scene::cpuBytes() const {
    size_t b = vertices.size() * sizeof(Vertex) + indices.size() * 4 + primitives.size() * sizeof(Primitive) +
               instances.size() * sizeof(Instance) + materials.size() * sizeof(Material);
    for (auto& t : textures) for (auto& m : t.cpuMips) b += m.size();
    return b;
}

}  // namespace gfx
