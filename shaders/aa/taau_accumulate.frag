#version 450
#include "aa/common_temporal.glsl"
// Educational temporal upscaler (TAAU, FSR2-style structure). Runs at OUTPUT resolution.
// Inputs at render resolution: HDR colour, velocity, depth. History at output resolution.
layout(location = 0) in vec2 vUV;                 // output uv
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uColor;     // render-res colour
layout(binding = 1) uniform sampler2D uHistory;   // output-res history, linear clamp
layout(binding = 2) uniform sampler2D uVelocity;  // render-res
layout(binding = 3) uniform sampler2D uDepth;     // render-res, reversed-Z
layout(binding = 4) uniform sampler2D uReactive;  // render-res R8 (0 = trust history), optional
layout(std140, binding = 3) uniform TaauParams {
    vec4  uRenderSize;    // xy render size (active), zw 1/size
    vec4  uOutputSize;    // xy output size, zw 1/size
    vec4  uJitterPx;      // xy jitter in render pixels (as applied to the projection)
    float uGamma;         // clip width
    float uMinAlpha;      // 0.04
    float uMaxAlpha;      // 0.25
    int   uReset;
    int   uUseReactive; int uDebugView; float uKernelScale; int _p1;   // kernel scale 1.0 = Lanczos-2 over +-2 output px
};
float lanczos2(float x) {
    x = abs(x); if (x >= 2.0) return 0.0; if (x < 1e-4) return 1.0;
    float px = 3.14159265 * x; return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}
void main() {
    ivec2 lim = ivec2(uRenderSize.xy) - 1;
    float ratio = uRenderSize.x * uOutputSize.z;                 // render / output (< 1)
    // continuous render-pixel coordinate of this output pixel centre, with the jitter removed:
    // a sample at render texel t represents scene position (t + 0.5 - jitter)
    vec2 rp = vUV * uRenderSize.xy - 0.5 + uJitterPx.xy;
    ivec2 base = ivec2(floor(rp));
    vec3 sum = vec3(0), m1 = vec3(0), m2 = vec3(0);
    vec3 bmin = vec3(1e9), bmax = vec3(-1e9);
    float wsum = 0.0, wmax = 0.0;
    // kernel radius widened by 1/ratio would be ideal; we use a 4x4 footprint in render space and
    // measure distances in OUTPUT pixels so near samples dominate (this is what turns jitter into detail)
    for (int y = -1; y <= 2; y++) for (int x = -1; x <= 2; x++) {
        ivec2 t = clamp(base + ivec2(x, y), ivec2(0), lim);
        vec3 c = rgbToYCoCg(tmWeight(texelFetch(uColor, t, 0).rgb));
        vec2 d = (vec2(base + ivec2(x, y)) - rp) / ratio;        // distance in output pixels
        // Lanczos-2 in OUTPUT pixels: a render sample only contributes to output pixels within ~1-2 px
        // of where it actually landed, so jittered samples add real detail instead of blur.
        float w = lanczos2(d.x * uKernelScale) * lanczos2(d.y * uKernelScale);
        w = max(w, 0.0);
        sum += c * w; wsum += w; wmax = max(wmax, w);
        if (abs(x - 0.5) < 1.0 && abs(y - 0.5) < 1.0) {           // inner 2x2 for the colour box
            bmin = min(bmin, c); bmax = max(bmax, c);
        }
        m1 += c; m2 += c * c;
    }
    vec3 cur = sum / max(wsum, 1e-4);
    vec3 mu = m1 / 16.0, sd = sqrt(max(m2 / 16.0 - mu * mu, 0.0));
    vec3 lo = min(mu - uGamma * sd, bmin), hi = max(mu + uGamma * sd, bmax);

    ivec2 rpi = clamp(ivec2(rp + 0.5), ivec2(0), lim);
    ivec2 bo = closestDepthOffset(uDepth, rpi);
    vec2 vel = velocityToUV(texelFetch(uVelocity, clamp(rpi + bo, ivec2(0), lim), 0).rg);
    vec2 prevUV = vUV - vel;
    vec3 histRaw = rgbToYCoCg(tmWeight(sampleCatmullRom(uHistory, prevUV)));
    vec3 hist = clipToAABB(histRaw, lo, hi);

    // confidence: a current sample landing right on this output pixel (wmax ~1) is worth a lot;
    // with 2x upscale most output pixels only get a near sample every few frames.
    float conf = clamp(wmax, 0.0, 1.0);
    float speed = length(vel * uOutputSize.xy);
    float a = mix(uMinAlpha, uMaxAlpha, conf * conf);
    a = mix(a, max(a, 0.5 * uMaxAlpha), clamp(speed / 16.0, 0.0, 1.0));
    if (uUseReactive != 0) a = mix(a, 0.9, texelFetch(uReactive, rpi, 0).r);
    if (uReset != 0 || outsideUV(prevUV)) a = 1.0;
    vec3 res = tmUnweight(yCoCgToRgb(mix(hist, cur, a)));
    if (uDebugView == 1) res = vec3(conf);
    else if (uDebugView == 2) res = vec3(clamp(length(histRaw - hist) * 4.0, 0.0, 1.0));
    oColor = vec4(max(res, vec3(0.0)), 1.0);
}
