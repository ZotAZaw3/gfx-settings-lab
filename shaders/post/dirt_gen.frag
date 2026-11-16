#version 450
// Procedural lens dirt (R8), regenerated on resize. See docs/lens_dirt.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
void main() {
    vec2 p = vUV * vec2(16.0 / 9.0, 1.0);
    float d = 0.0;
    for (int l = 0; l < 3; l++) {
        float s = 6.0 * pow(2.2, float(l));
        vec2 g = p * s, id = floor(g), f = fract(g) - 0.5;
        vec3 r = hash33(uvec3(uvec2(ivec2(id) + 1024), uint(l)));
        float rad = mix(0.1, 0.45, r.z);
        d += smoothstep(rad, 0.0, length(f - (r.xy - 0.5) * 0.5)) * step(0.55, r.x) * (0.6 / float(l + 1));
    }
    d += smoothstep(0.35, 0.6, valueNoise(p * vec2(3.0, 12.0)) * valueNoise(p * 5.0)) * 0.4;   // smudges
    d *= 0.5 + 0.5 * valueNoise(p * 2.0);
    oColor = vec4(clamp(d, 0.0, 1.0), 0, 0, 1);
}
