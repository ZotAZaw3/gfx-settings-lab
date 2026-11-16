#version 450
// PostOutput eye adaptation 1/2: (log2 luminance * weight, weight) at 1/4 res, centre-weighted metering.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
void main() {
    vec2 t = uOutputSize.zw;
    float L = 0.0;
    for (int y = -1; y <= 1; y += 2) for (int x = -1; x <= 1; x += 2)
        L += luma(textureLod(uSrc, vUV + vec2(x, y) * t * 1.5, 0.0).rgb);
    L *= 0.25;
    float w = mix(1.0, 1.0 - 0.9 * smoothstep(0.2, 0.75, length(vUV - 0.5)), pExposure2.z);
    oColor = vec4(log2(max(L, 1e-4)) * w, w, 0, 1);   // the 1x1 mip divides x by y
}
