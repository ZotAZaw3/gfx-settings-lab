#version 450
// PostRender DoF 4/4: blend the half-res blur over the sharp full-res image.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uBlur;
layout(binding = 2) uniform sampler2D uHalf;
void main() {
    if (pDof3.x > 0.5) {   // debug: near field blue, far field orange
        float c = textureLod(uHalf, vUV, 0.0).a;
        oColor = vec4(c < 0.0 ? vec3(0.1, 0.3, 1.0) * min(-c / 8.0, 1.0) : vec3(1.0, 0.5, 0.1) * min(c / 8.0, 1.0), 1.0);
        return;
    }
    vec4 b = textureLod(uBlur, vUV, 0.0);
    oColor = vec4(mix(textureLod(uSrc, vUV, 0.0).rgb, b.rgb, b.a), 1.0);
}
