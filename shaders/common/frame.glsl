// FrameConstants — UBO binding 0, bound for every pass. Mirrors src/core/frame.h.
#ifndef GFX_FRAME_GLSL
#define GFX_FRAME_GLSL

layout(std140, binding = 0) uniform FrameUBO {
    mat4 uView;
    mat4 uProj;                 // jittered
    mat4 uViewProj;             // jittered
    mat4 uInvView;
    mat4 uInvProj;
    mat4 uInvViewProj;
    mat4 uUnjitteredProj;
    mat4 uUnjitteredViewProj;
    mat4 uPrevViewProj;         // previous frame, unjittered
    mat4 uPrevView;
    vec4 uCameraPos;            // xyz, w near
    vec4 uCameraParams;         // x near, y far (draw distance), z fovY, w aspect
    vec4 uRenderSize;           // xy, zw = 1/xy
    vec4 uOutputSize;           // xy, zw = 1/xy
    vec4 uJitter;               // xy current (pixels), zw previous (pixels)
    vec4 uTime;                 // x time, y dt, z frame index, w render scale
    vec4 uSunDirection;         // xyz towards sun, w intensity
    vec4 uSunColor;             // rgb, w angular radius
    vec4 uDisplay;              // x exposure base (1), y gamma, z brightness, w paper white nits
    vec4 uDisplay2;             // x peak nits, y output mode (0 SDR,1 PQ,2 scRGB), z texture mip bias, w black level nits
    vec4 uCounts;               // x light count, y instance count, z material count
};

// Reversed-Z infinite projection: depth = near / viewDistance  (1 at near plane, 0 at infinity)
float linearDepth(float d) { return uCameraPos.w / max(d, 1e-7); }

// uv (0,0) = top-left on every backend. NDC y is up.
vec2 uvToNdc(vec2 uv) { return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }
vec2 ndcToUv(vec2 ndc) { return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); }

vec3 reconstructWorldPos(vec2 uv, float depth) {
    vec4 p = uInvViewProj * vec4(uvToNdc(uv), depth, 1.0);
    return p.xyz / p.w;
}
vec3 reconstructViewPos(vec2 uv, float depth) {
    vec4 p = uInvProj * vec4(uvToNdc(uv), depth, 1.0);
    return p.xyz / p.w;
}
// Velocity convention: NDC(current) - NDC(previous), unjittered.  prevUV = uv - velocity * vec2(0.5, -0.5)
vec2 velocityToPrevUv(vec2 uv, vec2 velocity) { return uv - velocity * vec2(0.5, -0.5); }

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

#endif
