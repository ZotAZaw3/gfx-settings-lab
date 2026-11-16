#version 450
// PostOutput bloom: 13-tap downsample (Jimenez 2014). pPass.x = 1 on the first pass: soft threshold + Karis average.
// See docs/bloom.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
vec3 S(vec2 o, vec2 t) { return min(textureLod(uSrc, vUV + o * t, 0.0).rgb, vec3(65000.0)); }
vec3 prefilter(vec3 c) {
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - pBloom.y, 0.0, pBloom.z);
    rq = pBloom.w * rq * rq;
    return c * max(rq, br - pBloom.x) / max(br, 1e-4);
}
float karis(vec3 c) { return 1.0 / (1.0 + luma(c)); }
void main() {
    vec2 t = 1.0 / vec2(textureSize(uSrc, 0));
    vec3 a = S(vec2(-2, -2), t), b = S(vec2(0, -2), t), c = S(vec2(2, -2), t);
    vec3 d = S(vec2(-1, -1), t), e = S(vec2(1, -1), t);
    vec3 f = S(vec2(-2, 0), t), g = S(vec2(0, 0), t), h = S(vec2(2, 0), t);
    vec3 i = S(vec2(-1, 1), t), j = S(vec2(1, 1), t);
    vec3 k = S(vec2(-2, 2), t), l = S(vec2(0, 2), t), m = S(vec2(2, 2), t);
    vec3 o;
    if (pPass.x > 0.5) {
        vec3 g0 = (d + e + i + j) * 0.25, g1 = (a + b + f + g) * 0.25, g2 = (b + c + g + h) * 0.25,
             g3 = (f + g + k + l) * 0.25, g4 = (g + h + l + m) * 0.25;
        float w0 = karis(g0) * 0.5, w1 = karis(g1) * 0.125, w2 = karis(g2) * 0.125, w3 = karis(g3) * 0.125, w4 = karis(g4) * 0.125;
        o = prefilter((g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4));
    } else {
        o = (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f + h + l) * 0.0625 + g * 0.125;
    }
    oColor = vec4(max(o, vec3(0)), 1);
}
