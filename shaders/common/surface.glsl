// Surface description, shading model ids and the light list (SSBO 0).
// Safe to include from compute/fullscreen passes: declares no samplers.
#ifndef GFX_SURFACE_GLSL
#define GFX_SURFACE_GLSL
#include "common/frame.glsl"

struct GpuLight {
    vec4 positionRange;
    vec4 colorIntensity;
    vec4 directionType;      // w: 0 point, 1 spot
    vec4 cone;               // x cos inner, y cos outer, z casts shadows, w shadow index
};
layout(std430, binding = 0) readonly buffer LightSSBO { GpuLight lights[]; };

#define SHADING_DEFAULT    0
#define SHADING_SUBSURFACE 1
#define SHADING_FOLIAGE    2
#define SHADING_UNLIT      3
#define SHADING_CLEARCOAT  4

// Surface — what the G-buffer stores and what lighting/shade.glsl consumes.
struct Surface {
    vec3  position;      // world
    vec3  normal;        // world, normalized
    vec3  albedo;
    float roughness;
    float metallic;
    float ao;            // material AO
    vec3  emissive;
    int   shadingModel;
    float sssMask;       // subsurface strength / thickness
};
#endif
