#version 450
#include "aa/common_temporal.glsl"
// "RR-lite": educational joint denoise + temporal upscale, standing in for DLSS Ray Reconstruction.
// Instead of [denoiser -> upscaler] (two blurs, two histories), the noisy demodulated RT signal is
// accumulated directly in an OUTPUT-resolution history. History is rejected by geometry guides
// (depth + normal) rather than by a tight colour box, because the colour box of a noisy signal is
// meaningless. A light 3x3 guided spatial filter covers the first frames after disocclusion.
layout(location = 0) in vec2 vUV;                 // output uv
layout(location = 0) out vec4 oHistory;           // rgb = accumulated demodulated (tm-weighted), a = history length
layout(location = 1) out vec4 oGuide;             // xyz = normal, w = depth (output res, next frame's prev guide)
layout(location = 2) out vec4 oColor;             // final remodulated HDR radiance
layout(binding = 0) uniform sampler2D uNoisy;     // render-res NOISY scene colour (RT denoiser skipped in this mode)
layout(binding = 1) uniform sampler2D uHistory;   // output-res: rgb demodulated accumulated, a = history length
layout(binding = 2) uniform sampler2D uVelocity;  // render-res
layout(binding = 3) uniform sampler2D uDepth;     // render-res
layout(binding = 4) uniform sampler2D uNormal;    // render-res world normal
layout(binding = 5) uniform sampler2D uPrevGuide; // output-res previous guide
layout(binding = 6) uniform sampler2D uAlbedo;    // render-res albedo for remodulation
layout(std140, binding = 3) uniform RrParams {
    vec4 uRenderSize; vec4 uOutputSize; vec4 uJitterPx;
    float uMaxHistory; float uDepthTol; int uNormalEncoded; int uReset;
    int uDebugView; int _p0, _p1, _p2;
};
vec3 N(ivec2 p) { vec3 n = texelFetch(uNormal, p, 0).xyz; return normalize(uNormalEncoded != 0 ? n * 2.0 - 1.0 : n); }
void main() {
    ivec2 lim = ivec2(uRenderSize.xy) - 1;
    vec2 rp = vUV * uRenderSize.xy - 0.5 + uJitterPx.xy;
    ivec2 rpi = clamp(ivec2(floor(rp + 0.5)), ivec2(0), lim);
    vec3 nC = N(rpi); float zC = texelFetch(uDepth, rpi, 0).r;
    // guided 3x3 gather of the noisy signal (edge-stopping on depth and normal)
    vec3 cur = vec3(0); float ws = 0.0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        ivec2 q = clamp(rpi + ivec2(x, y), ivec2(0), lim);
        float wz = exp(-abs(zC - texelFetch(uDepth, q, 0).r) / (uDepthTol * max(zC, 1e-6) + 1e-7));
        float wn = pow(max(dot(nC, N(q)), 0.0), 32.0);
        float wd = (x == 0 && y == 0) ? 1.0 : 0.5;
        float w = wz * wn * wd;
        vec3 alb = max(texelFetch(uAlbedo, q, 0).rgb, vec3(0.02));           // demodulate: keep texture detail out of the blur
        cur += tmWeight(texelFetch(uNoisy, q, 0).rgb / alb) * w; ws += w;
    }
    cur /= max(ws, 1e-5);
    vec2 prevUV = vUV - velocityToUV(texelFetch(uVelocity, rpi, 0).rg);
    float histLen = 1.0; vec3 acc = cur;
    bool valid = uReset == 0 && !outsideUV(prevUV);
    if (valid) {
        vec4 pg = texture(uPrevGuide, prevUV);
        valid = abs(pg.w - zC) <= uDepthTol * max(zC, pg.w) && dot(normalize(pg.xyz), nC) > 0.85;
    }
    if (valid) {
        vec4 h = texture(uHistory, prevUV);
        histLen = min(h.a + 1.0, uMaxHistory);
        acc = mix(h.rgb, cur, 1.0 / histLen);         // running average, capped
    }
    vec3 albedo = texelFetch(uAlbedo, rpi, 0).rgb;
    vec3 res = tmUnweight(acc) * max(albedo, vec3(0.02));                // remodulate
    if (uDebugView == 1) res = vec3(histLen / uMaxHistory);
    oHistory = vec4(acc, histLen);
    oColor = vec4(res, 1.0);
    oGuide = vec4(nC, zC);
}
