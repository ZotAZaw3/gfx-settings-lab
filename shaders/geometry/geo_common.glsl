// geometry/geo_common.glsl - shared helpers for the geometry module (includes common/frame.glsl).
// gfxlab conventions: D3D clip (z 0..1), uv (0,0) top-left, reversed-Z infinite (near=1, far=0).
#ifndef GEO_COMMON_GLSL
#define GEO_COMMON_GLSL
#include "common/frame.glsl"

float hash11(float p){ p = fract(p * 0.1031); p *= p + 33.33; p *= p + p; return fract(p); }
float hash12(vec2 p){ vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
vec2  hash22(vec2 p){ vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973)); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.xx + p3.yz) * p3.zy); }
float hashU(uint n){ n = (n << 13u) ^ n; n = n * (n * n * 15731u + 789221u) + 1376312589u; return float(n & 0x7fffffffu) / float(0x7fffffff); }

float valueNoise(vec2 p){
    vec2 i = floor(p), f = fract(p); vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p){ float a = 0.5, s = 0.0; for (int i = 0; i < 5; ++i){ s += a * valueNoise(p); p *= 2.03; a *= 0.5; } return s; }

// 4x4 ordered dither threshold in (0,1)
float bayer4(ivec2 p){
    const float m[16] = float[](0.,8.,2.,10.,12.,4.,14.,6.,3.,11.,1.,9.,15.,7.,13.,5.);
    return (m[(p.x & 3) + (p.y & 3) * 4] + 0.5) / 16.0;
}
#endif
