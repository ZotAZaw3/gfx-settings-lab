// Shared declarations for the post-processing module (see docs/post_processing_overview.md).
// Engine conventions: uv (0,0) = top-left, reversed-Z infinite depth (linearDepth = near / d),
// scene.velocity = NDC(current) - NDC(previous). FrameConstants come from common/frame.glsl (UBO 0).
#ifndef POST_COMMON_GLSL
#define POST_COMMON_GLSL
#include "common/frame.glsl"

// All post settings, updated once per frame (mirrors PostParams in src/modules/post/post_params.h).
layout(std140, binding = 3) uniform PostUBO {
    vec4 pHaze;         // strength, speed, scale, start (m)
    vec4 pHaze2;        // end (m), -, -, -
    vec4 pDof;          // focal length mm, f-stop, sensor height mm, max CoC (half-res px)
    vec4 pDof2;         // manual focus m, auto focus (0/1), bokeh boost, sample count
    vec4 pDof3;         // debug CoC, focus speed, -, -
    vec4 pMB;           // shutter fraction, max blur px, samples, camera only (0/1)
    vec4 pPanini;       // distance, crop, tan(fovX/2), tan(fovY/2)
    vec4 pExposure;     // compensation EV, min EV, max EV, key (middle grey)
    vec4 pExposure2;    // speed up, speed down, centre weight, auto (0/1)
    vec4 pExposure3;    // luminance max mip, reset (0/1), -, -
    vec4 pRays;         // density, decay, weight, samples
    vec4 pRays2;        // intensity, sun disc radius, enabled, -
    vec4 pBloom;        // threshold, threshold - knee, 2 * knee, 0.25 / knee
    vec4 pBloom2;       // intensity, radius (texels), lens dirt intensity, enabled
    vec4 pBloomTint;    // rgb tint, -
    vec4 pFlare;        // intensity, ghost spacing, halo width, chromatic distortion
    vec4 pFlare2;       // ghost count, enabled, -, -
    vec4 pGlare;        // intensity, streak count, size, anamorphic
    vec4 pGlare2;       // enabled, -, -, -
    vec4 pSun;          // sun uv x, sun uv y, sun in front of camera (0/1), aspect (output)
    vec4 pSunRadiance;  // rgb sun radiance (color * intensity), -
    vec4 pRain;         // amount, drop size, speed, fog
    vec4 pRain2;        // layers, -, -, -
    vec4 pTonemap;      // operator, white point, hdr output (0/1), -
    vec4 pDisplay;      // paper white nits, peak nits, peak / paper white, base exposure
    vec4 pGrade;        // temperature, tint, contrast, saturation
    vec4 pGrade2;       // vibrance, hue shift (rad), LUT strength, LUT enabled
    vec4 pLift;         // rgb, -
    vec4 pGamma;        // rgb, -
    vec4 pGain;         // rgb, -
    vec4 pCA;           // intensity, samples, enabled, -
    vec4 pVignette;     // intensity, smoothness, roundness, enabled
    vec4 pVignetteColor;// rgb, -
    vec4 pGrain;        // intensity, size px, luminance response, enabled
    vec4 pGrain2;       // colored, -, -, -
    vec4 pLetterbox;    // bar fraction, show LUT, debug view, LUT size
    vec4 pLetterboxColor;
    vec4 pCalib;        // step (0 peak, 1 paper white, 2 black), test nits, active, -
};

// Per-draw values inside one pass (mip index, prefilter flag, LUT slice...).
layout(std140, binding = 4) uniform PassUBO {
    vec4 pPass;         // meaning depends on the pass, documented in each shader
};

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
float luma(vec3 c) { return dot(c, LUMA); }
float frameTime()  { return uTime.x; }
float frameDt()    { return uTime.y; }
uint  frameIndex() { return uint(uTime.z); }

// PCG-style integer hash -> [0,1)
uint pcg(uint v) { uint s = v * 747796405u + 2891336453u; uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return (w >> 22u) ^ w; }
float hash12(uvec2 p) { return float(pcg(p.x + pcg(p.y))) / 4294967296.0; }
float hash13(uvec3 p) { return float(pcg(p.x + pcg(p.y + pcg(p.z)))) / 4294967296.0; }
vec3  hash33(uvec3 p) { uint a = pcg(p.x + pcg(p.y + pcg(p.z))); uint b = pcg(a); uint c = pcg(b);
                        return vec3(float(a), float(b), float(c)) / 4294967296.0; }
float hash21f(vec2 p) { return hash12(uvec2(ivec2(floor(p)) + ivec2(32768))); }

float valueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21f(i), hash21f(i + vec2(1, 0)), u.x),
               mix(hash21f(i + vec2(0, 1)), hash21f(i + vec2(1, 1)), u.x), u.y);
}

#endif
