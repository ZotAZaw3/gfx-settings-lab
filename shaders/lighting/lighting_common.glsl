// lighting_common.glsl - shared declarations for every lighting pass.
// Engine conventions (docs/MODULE_API.md): D3D clip space, uv (0,0) = top-left, REVERSED-Z with an
// infinite far plane (near = 1, far/sky = 0). FrameConstants come from common/frame.glsl (UBO 0).
// Mirrors src/modules/lighting/LightingShared.h (LightingUBO). Keep both in sync.
#ifndef LIGHTING_COMMON_GLSL
#define LIGHTING_COMMON_GLSL
#include "common/frame.glsl"

// UBO 6: lighting settings, filled once per frame by lighting.pbr (FrameSetup, order 1001).
layout(std140, binding = 6) uniform LightingUBO {
    mat4  uCascadeVP[4];
    vec4  uCascadeSplits;     // view distances where each cascade ends
    vec4  uCascadeTexel;      // world size of one shadow texel per cascade
    ivec4 uShadowCfg;         // x cascades (0 = off), y filter (0 hw, 1 pcf3x3, 2 poisson, 3 pcss), z contact on, w capsule count
    vec4  uShadowParams;      // x normal offset (texels), y light size (pcss), z max shadow distance, w cascade blend band
    ivec4 uAOCfg;             // x technique (0 off,1 ssao,2 hbao,3 gtao), y samples/dirs/slices, z steps, w multibounce
    vec4  uAOParams;          // x radius (m), y intensity, z bias, w specular occlusion on
    ivec4 uReflCfg;           // x env cube on, y ssr on, z ssr steps, w planar on
    vec4  uReflParams;        // x max ssr roughness, y thickness (m), z env max mip, w planar height (world y)
    ivec4 uGICfg;             // x technique (0 const,1 hemi+ibl,2 probes L1,3 probes L2), y ssgi on, z ssgi rays, w ssgi steps
    vec4  uGIParams;          // x ssgi intensity, y ssgi ray length (m), z probe normal bias (m), w temporal alpha
    vec4  uProbeGridMin;      // xyz
    vec4  uProbeGridSize;     // xyz extent
    ivec4 uProbeGridDim;      // xyz probe counts, w = 1 when baked
    ivec4 uSSSCfg;            // x technique (0 off,1 wrap,2 separable), y taps, z transmittance, w unused
    vec4  uSSSParams;         // x width (m), y strength, z wrap amount, w unused
    ivec4 uLightCfg;          // x brdf (0 blinn, 1 ggx), y max lights, z multiscatter, w area approx
    ivec4 uLightCfg2;         // x light count in scene, y tiled culling, z sunVisibility target present, w clearcoat on
    vec4  uAmbient;           // rgb sky ambient, w ibl intensity
    vec4  uGround;            // rgb ground ambient, w emissive intensity
    vec4  uReflBoxMin;        // local probe box proxy, w = parallax correction on
    vec4  uReflBoxMax;
    vec4  uReflProbePos;
    vec4  uCapsuleA[8];       // xyz endpoint a, w radius
    vec4  uCapsuleB[8];       // xyz endpoint b
};

const float PI = 3.14159265359;

int   frameIndex() { return int(uTime.z); }
vec3  sunRadiance() { return uSunColor.rgb * uSunDirection.w; }
vec3  sunDir() { return normalize(uSunDirection.xyz); }

vec3 viewPosFromDepth(vec2 uv, float depth) { return reconstructViewPos(uv, max(depth, 1e-7)); }
vec3 worldPosFromDepth(vec2 uv, float depth) { return reconstructWorldPos(uv, max(depth, 1e-7)); }
// view space is right-handed, camera looks down -Z. Returns (uv, ndc depth).
vec3 projectView(vec3 vp) {
    vec4 c = uProj * vec4(vp, 1.0);
    c.xyz /= c.w;
    return vec3(ndcToUv(c.xy), c.z);
}
bool isSky(float depth) { return depth <= 0.0; }   // reversed-Z: far plane / sky = 0

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
// Interleaved gradient noise, rotated per frame (pairs well with TAA)
float ign(vec2 pixel, int frame) {
    pixel += 5.588238 * float(frame & 63);
    return fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}
vec2 hammersley(uint i, uint n) {
    uint b = i;
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(n), float(b) * 2.3283064365386963e-10);
}
mat3 tangentFrame(vec3 n) {
    vec3 up = abs(n.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 t = normalize(cross(up, n));
    return mat3(t, cross(n, t), n);
}
vec3 cosineSample(vec2 u) {
    float r = sqrt(u.x), phi = 2.0 * PI * u.y;
    return vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}
// Cube texel -> direction, by memory row (independent of clip-space conventions).
vec3 cubeDir(ivec3 id, float size) {
    vec2 uv = (vec2(id.xy) + 0.5) / size * 2.0 - 1.0;
    switch (id.z) {
        case 0: return normalize(vec3( 1, -uv.y, -uv.x));
        case 1: return normalize(vec3(-1, -uv.y,  uv.x));
        case 2: return normalize(vec3( uv.x,  1,  uv.y));
        case 3: return normalize(vec3( uv.x, -1, -uv.y));
        case 4: return normalize(vec3( uv.x, -uv.y,  1));
        default:return normalize(vec3(-uv.x, -uv.y, -1));
    }
}

#endif
