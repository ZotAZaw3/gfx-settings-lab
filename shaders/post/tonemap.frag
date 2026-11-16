#version 450
// ToneMap stage: exposure (manual EV + eye adaptation) and the tonemapping curve.
// Output is display-referred linear: SDR 0..1, HDR in units of paper white up to peak/paperWhite.
// The core OutputTransform applies brightness, gamma/sRGB or PQ afterwards. See docs/tonemapping_hdr.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uEV;     // 1x1 adapted EV

vec3 reinhard(vec3 x) { return x / (1.0 + x); }
vec3 reinhardExt(vec3 x, float W) { return x * (1.0 + x / (W * W)) / (1.0 + x); }
vec3 hableP(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 hable(vec3 x, float W) { return hableP(x * 2.0) / hableP(vec3(W)); }
vec3 acesFitted(vec3 c) {   // Stephen Hill's RRT+ODT fit
    const mat3 inM  = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outM = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    c = inM * c;
    vec3 a = c * (c + 0.0245786) - 0.000090537;
    vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    return clamp(outM * (a / b), 0.0, 1.0);
}
vec3 agxContrast(vec3 x) {  // AgX default contrast, 6th order fit (B. Wrensch)
    vec3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 agx(vec3 c) {
    const mat3 inM = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                          0.0784335999999992, 0.878468636469772, 0.0784336,
                          0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outM = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                           -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                           -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = inM * max(c, vec3(1e-10));
    c = clamp((log2(c) - minEv) / (maxEv - minEv), 0.0, 1.0);
    c = outM * agxContrast(c);
    return pow(max(c, vec3(0)), vec3(2.2));     // AgX curve output is display-encoded; return linear
}
vec3 tonemap(vec3 x) {
    int op = int(pTonemap.x); float W = pTonemap.y;
    if (op == 0) return min(x, vec3(1));
    if (op == 1) return reinhard(x);
    if (op == 2) return reinhardExt(x, W);
    if (op == 3) return hable(x, W);
    if (op == 4) return acesFitted(x);
    return agx(x);
}
void main() {
    vec3 c = textureLod(uSrc, vUV, 0.0).rgb;
    float ev = pExposure.x;
    if (pExposure2.w > 0.5) ev += textureLod(uEV, vec2(0.5), 0.0).r;
    c *= exp2(ev) * pDisplay.w * uDisplay.x;      // uDisplay.x: core base exposure, stays 1
    if (pTonemap.z > 0.5) {
        float peak = pDisplay.z;                   // peak nits / paper white nits
        c = tonemap(c / peak) * peak;              // shoulder ends at the display peak instead of 1.0
    } else {
        c = tonemap(c);
    }
    oColor = vec4(max(c, vec3(0)), 1);
}
