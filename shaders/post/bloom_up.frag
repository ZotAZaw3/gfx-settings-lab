#version 450
// PostOutput bloom: 3x3 tent upsample, drawn with additive blending into the next larger mip.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
void main() {
    vec2 t = pBloom2.y / vec2(textureSize(uSrc, 0));
    vec3 s = textureLod(uSrc, vUV + vec2(-t.x, -t.y), 0.0).rgb + textureLod(uSrc, vUV + vec2(0, -t.y), 0.0).rgb * 2.0
           + textureLod(uSrc, vUV + vec2(t.x, -t.y), 0.0).rgb  + textureLod(uSrc, vUV + vec2(-t.x, 0), 0.0).rgb * 2.0
           + textureLod(uSrc, vUV, 0.0).rgb * 4.0              + textureLod(uSrc, vUV + vec2(t.x, 0), 0.0).rgb * 2.0
           + textureLod(uSrc, vUV + vec2(-t.x, t.y), 0.0).rgb  + textureLod(uSrc, vUV + vec2(0, t.y), 0.0).rgb * 2.0
           + textureLod(uSrc, vUV + vec2(t.x, t.y), 0.0).rgb;
    oColor = vec4(s / 16.0, 1);
}
