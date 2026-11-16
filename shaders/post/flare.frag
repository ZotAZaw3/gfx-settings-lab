#version 450
// PostOutput lens flare: screen-space ghosts + halo with chromatic distortion (Chapman 2013).
// Reads the thresholded half-res bloom image. See docs/lens_flare_sun_glare.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uBright;
vec3 chroma(vec2 uv, vec2 dir) {
    float ca = pFlare.w;
    return vec3(textureLod(uBright, uv + dir * ca, 0.0).r, textureLod(uBright, uv, 0.0).g, textureLod(uBright, uv - dir * ca, 0.0).b);
}
void main() {
    float aspect = pSun.w;
    vec2 uv = vec2(1.0) - vUV;                         // flip through the centre
    vec2 ghostVec = (vec2(0.5) - uv) * pFlare.y;
    vec2 dir = normalize(ghostVec + 1e-6);
    vec3 acc = vec3(0);
    int N = int(pFlare2.x);
    for (int i = 0; i < N; i++) {
        vec2 o = fract(uv + ghostVec * float(i));
        float w = pow(max(1.0 - length(vec2(0.5) - o) / 0.7071, 0.0), 10.0);
        vec3 tint = mix(vec3(1.0, 0.8, 0.6), vec3(0.6, 0.8, 1.0), fract(float(i) * 0.37));
        acc += chroma(o, dir) * w * tint;
    }
    vec2 hv = vec2(0.5) - uv; hv.x *= aspect;
    vec2 hdir = normalize(hv + 1e-6); hdir.x /= aspect;
    vec2 ho = fract(uv + hdir * pFlare.z);
    float hd = length((vec2(0.5) - ho) * vec2(aspect, 1.0));
    acc += chroma(ho, dir) * pow(max(1.0 - abs(hd - pFlare.z) / 0.15, 0.0), 4.0) * 0.6;
    oColor = vec4(acc, 1);
}
