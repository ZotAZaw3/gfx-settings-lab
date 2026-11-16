#version 450
// PostRender DoF 3/4: small tent filter that hides gather undersampling.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
void main() {
    vec2 t = 1.0 / vec2(textureSize(uSrc, 0));
    oColor = (textureLod(uSrc, vUV + vec2(-0.5, -0.5) * t, 0.0) + textureLod(uSrc, vUV + vec2(0.5, -0.5) * t, 0.0)
            + textureLod(uSrc, vUV + vec2(-0.5, 0.5) * t, 0.0) + textureLod(uSrc, vUV + vec2(0.5, 0.5) * t, 0.0)) * 0.25;
}
