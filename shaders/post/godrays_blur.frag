#version 450
// PostOutput god rays 2/2: radial blur toward the sun (Mitchell, GPU Gems 3 ch.13).
// pPass.x = step scale (pass 1: 1, pass 2: 1/samples -> 2 passes give samples^2 effective taps)
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
void main() {
    int N = int(pRays.w);
    vec2 delta = (vUV - pSun.xy) * pRays.x * pPass.x / float(N);
    float j = hash13(uvec3(uvec2(gl_FragCoord.xy), frameIndex()));
    vec2 uv = vUV - delta * j;
    float illum = 1.0; vec3 sum = vec3(0);
    for (int i = 0; i < N; i++) {
        uv -= delta;
        sum += textureLod(uSrc, uv, 0.0).rgb * illum * pRays.z;
        illum *= pRays.y;
    }
    oColor = vec4(sum / float(N) * 4.0, 1);
}
