#version 450
// Fallback ACES fit (Narkowicz), used only when the post module provides no ToneMap pass.
#include "common/frame.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D tSrc;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
    vec3 c = textureLod(tSrc, vUV, 0).rgb * uDisplay.x;
    oColor = vec4(aces(c * 0.6), 1.0);
}
