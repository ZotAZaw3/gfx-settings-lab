#version 450
// PostRender: heat haze / screen distortion. See docs/heat_haze_distortion.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uDepth;
void main() {
    float strength = pHaze.x, speed = pHaze.y, scale = pHaze.z, start = pHaze.w, end = pHaze2.x;
    float t = frameTime();
    float z = linearDepth(textureLod(uDepth, vUV, 0.0).r);
    float m = smoothstep(start, end, z) * strength;
    m *= smoothstep(0.15, 0.6, vUV.y);              // haze hugs the ground / horizon
    vec2 p = vUV * scale;
    vec2 n = vec2(valueNoise(p + vec2(0.0, t * speed)) + 0.5 * valueNoise(p * 2.3 + vec2(t * speed * 1.3, 0.0)),
                  valueNoise(p * 1.7 - vec2(t * speed * 0.7, 0.0)) + 0.5 * valueNoise(p * 3.1 - vec2(0.0, t * speed))) / 1.5 - 0.5;
    vec2 suv = vUV + n * m * 0.012;
    // foreground guard: never pull nearer pixels into the haze
    if (linearDepth(textureLod(uDepth, suv, 0.0).r) < z * 0.9) suv = vUV;
    oColor = vec4(textureLod(uSrc, suv, 0.0).rgb, 1.0);
}
