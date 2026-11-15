// Scene draw interface: vertex layout, DrawConstants (UBO 1), materials (SSBO 1), lights (SSBO 0).
// Mirrors src/core/renderer.h and src/core/scene.h.
#ifndef GFX_DRAW_GLSL
#define GFX_DRAW_GLSL
#include "common/frame.glsl"
#include "common/surface.glsl"

layout(std140, binding = 1) uniform DrawUBO {
    mat4  uWorld;
    mat4  uPrevWorld;
    mat4  uPassViewProj;     // camera (jittered) or shadow cascade view-projection
    mat4  uPassPrevViewProj; // unjittered previous (velocity)
    uvec4 uIds;              // x material, y instance, z primitive, w lod
    vec4  uDrawParams;       // x mip bias, y alpha test
};

struct GpuMaterial {
    vec4  baseColor;
    vec4  emissiveRough;     // rgb emissive, a roughness
    vec4  params;            // x metallic, y normalScale, z occlusionStrength, w alphaCutoff
    vec4  sss;               // rgb subsurface color, a radius
    ivec4 tex0;              // baseColor, normal, metalRough, occlusion (-1 = none)
    ivec4 tex1;              // emissive, height, alphaMode, shadingModel
    vec4  params2;           // x displacementScale, y doubleSided
};
layout(std430, binding = 1) readonly buffer MaterialSSBO { GpuMaterial materials[]; };

// Material textures bound by SceneRenderer (fallback 1x1 textures when a map is missing)
layout(binding = 0) uniform sampler2D tBaseColor;
layout(binding = 1) uniform sampler2D tNormalMap;
layout(binding = 2) uniform sampler2D tMetalRough;   // glTF: g roughness, b metallic
layout(binding = 3) uniform sampler2D tOcclusion;
layout(binding = 4) uniform sampler2D tEmissive;
layout(binding = 5) uniform sampler2D tHeight;

#endif
