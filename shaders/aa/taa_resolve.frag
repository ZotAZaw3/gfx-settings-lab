#version 450
#include "aa/common_temporal.glsl"
// TAA resolve at render resolution == output resolution.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uCurrent;   // scene.color (HDR), point/linear clamp
layout(binding = 1) uniform sampler2D uHistory;   // previous TAA output, linear clamp
layout(binding = 2) uniform sampler2D uVelocity;  // scene.velocity (NDC delta)
layout(binding = 3) uniform sampler2D uDepth;     // scene.depth (reversed-Z)
layout(std140, binding = 3) uniform TaaParams {
    vec4  uJitterUV;       // xy = current jitter in UV units (to unjitter current sample)
    float uGamma;          // variance clip width, 1.0 - 1.25
    float uMinAlpha;       // 0.06 - 0.1  (history weight = 1 - alpha)
    float uMaxAlpha;       // 0.3
    int   uReset;          // 1 = discard history this frame
    int   uDebugView;      // 0 off, 1 history only, 2 clip amount, 3 velocity
};
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 lim = textureSize(uCurrent, 0) - 1;
    vec2 px = 1.0 / vec2(textureSize(uCurrent, 0));
    ivec2 bo = closestDepthOffset(uDepth, p);
    vec2 vel = velocityToUV(texelFetch(uVelocity, clamp(p + bo, ivec2(0), lim), 0).rg);

    vec3 m1 = vec3(0), m2 = vec3(0), cur = vec3(0);
    vec3 bmin = vec3(1e9), bmax = vec3(-1e9);
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        vec3 c = rgbToYCoCg(tmWeight(texelFetch(uCurrent, clamp(p + ivec2(x, y), ivec2(0), lim), 0).rgb));
        if (x == 0 && y == 0) cur = c;
        m1 += c; m2 += c * c; bmin = min(bmin, c); bmax = max(bmax, c);
    }
    vec3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0));
    vec3 lo = max(mu - uGamma * sigma, bmin), hi = min(mu + uGamma * sigma, bmax);

    vec2 prevUV = vUV - vel;
    vec3 histRaw = rgbToYCoCg(tmWeight(sampleCatmullRom(uHistory, prevUV)));
    vec3 hist = clipToAABB(histRaw, lo, hi);
    float clipAmt = clamp(length(histRaw - hist) * 4.0, 0.0, 1.0);

    float speed = length(vel / px);
    float a = mix(uMinAlpha, uMaxAlpha, clamp(speed / 8.0, 0.0, 1.0));
    a = mix(a, uMaxAlpha, clipAmt * 0.5);
    if (uReset != 0 || outsideUV(prevUV)) a = 1.0;
    vec3 res = tmUnweight(yCoCgToRgb(mix(hist, cur, a)));

    if (uDebugView == 1) res = tmUnweight(yCoCgToRgb(hist));
    else if (uDebugView == 2) res = vec3(clipAmt, 0.0, 1.0 - clipAmt);
    else if (uDebugView == 3) res = vec3(abs(vel) * 50.0, 0.0);
    oColor = vec4(max(res, vec3(0.0)), 1.0);
}
