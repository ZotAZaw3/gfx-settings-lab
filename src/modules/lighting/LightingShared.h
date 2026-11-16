// LightingShared.h - state shared by the lighting modules (shadows, AO, reflections, GI, SSS and the
// PBR hub). Each feature is its own gfx::Module so the profiler attributes GPU time and VRAM per
// setting; they talk through this struct. Only lighting.pbr uploads the UBO and binds resources.
#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include "core/engine.h"

namespace gfx::lighting {

// std140 mirror of LightingUBO in shaders/lighting/lighting_common.glsl (UBO binding 6).
struct LightingUBO {
    glm::mat4  cascadeVP[4];
    glm::vec4  cascadeSplits;
    glm::vec4  cascadeTexel;
    glm::ivec4 shadowCfg;      // cascades, filter, contact, capsule count
    glm::vec4  shadowParams;   // normal offset, light size, max distance, blend band
    glm::ivec4 aoCfg;          // technique, samples, steps, multibounce
    glm::vec4  aoParams;       // radius, intensity, bias, specular occlusion
    glm::ivec4 reflCfg;        // env on, ssr on, ssr steps, planar on
    glm::vec4  reflParams;     // max ssr roughness, thickness, env max mip, planar height
    glm::ivec4 giCfg;          // technique, ssgi on, rays, steps
    glm::vec4  giParams;       // ssgi intensity, ray length, probe normal bias, temporal alpha
    glm::vec4  probeGridMin;
    glm::vec4  probeGridSize;
    glm::ivec4 probeGridDim;   // xyz, w baked
    glm::ivec4 sssCfg;         // technique, taps, transmittance
    glm::vec4  sssParams;      // width, strength, wrap
    glm::ivec4 lightCfg;       // brdf, max lights, multiscatter, area approx
    glm::ivec4 lightCfg2;      // light count, tiled, sunVisibility present, clearcoat
    glm::vec4  ambient;        // rgb sky ambient, w ibl intensity
    glm::vec4  ground;         // rgb ground ambient, w emissive intensity
    glm::vec4  reflBoxMin;     // w parallax on
    glm::vec4  reflBoxMax;
    glm::vec4  reflProbePos;
    glm::vec4  capsuleA[8];
    glm::vec4  capsuleB[8];
};
static_assert(sizeof(LightingUBO) % 16 == 0, "std140");

constexpr int kBrdfLutSize = 32;              // matches BRDF_LUT_SIZE in shade.glsl
constexpr int kMaxCascades = 4;

struct Shared {
    LightingUBO ubo{};

    // shadows (lighting.shadows)
    rhi::Texture* shadowArray = nullptr;      // D32F Tex2DArray, standard Z
    rhi::Sampler* shadowCmp = nullptr;        // LessEqual compare
    rhi::Sampler* shadowRaw = nullptr;
    // reflections (lighting.reflections)
    rhi::Texture* envCube = nullptr;          // prefiltered GGX mips
    rhi::Texture* planar = nullptr;           // planar reflection color (nullptr = off)
    bool ssrNeedsHiZ = false;
    // GI (lighting.gi)
    rhi::Buffer* probeSSBO = nullptr;
    bool ssgiNeedsHiZ = false;
    // SSS (lighting.sss)
    rhi::Texture* sssDiffuse = nullptr;       // written by the resolve for SSS pixels (nullptr = off)
    // hub (lighting.pbr)
    rhi::Buffer*  brdfLut = nullptr;
    rhi::Texture* hiz = nullptr;              // closest-depth pyramid (built when SSR/SSGI need it)
    rhi::Texture* prevColor = nullptr;        // last frame's lit HDR with mips (SSR / SSGI source)
    bool          prevColorValid = false;
    rhi::Texture* white = nullptr;            // 1x1 RGBA8 white
    rhi::Texture* blackZeroAlpha = nullptr;   // 1x1 RGBA16F (0,0,0,0)
    rhi::Texture* dummyStorage = nullptr;     // 1x1 RGBA16F storage (image writes to it are discarded)

    bool msaaForward = false;
};

Shared& shared();

// Binds every shade.glsl resource (tex 8-15, UBO 6, SSBO 4-5). `neutralScreen` binds 1x1 fallbacks
// for the screen-space inputs, for passes whose camera is not the main view (planar, probes).
void bindShading(FrameContext& ctx, bool neutralScreen);

inline uint32_t groups(uint32_t n, uint32_t g) { return (n + g - 1) / g; }

// Common texture descriptor for a render-res compute target.
inline rhi::TextureDesc storageDesc(rhi::Format f, const char* name, const char* owner) {
    rhi::TextureDesc d;
    d.format = f;
    d.usage = rhi::TexUsageSampled | rhi::TexUsageStorage;
    d.name = name;
    d.owner = owner;
    return d;
}

// Is a ray-traced replacement for this input running? (ids from the ray tracing thread)
inline bool settingOn(FrameContext& ctx, const char* id) {
    auto* s = ctx.settings.find(id);
    return s && s->asNumber() != 0.0f;
}

// Cube face basis matching cubeDir() in lighting_common.glsl (by memory row, uv top-left).
// Returns a view-projection for a 90 degree reversed-Z camera at `eye` looking at face `face`.
glm::mat4 cubeFaceViewProj(int face, glm::vec3 eye, float nearPlane);

}  // namespace gfx::lighting
