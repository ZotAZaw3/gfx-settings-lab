#version 450
// Composite the (possibly low-res) premultiplied effects buffer onto scene.color (blend: ONE, ONE_MINUS_SRC_ALPHA).
layout(binding = 0) uniform sampler2D tFx;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main(){ outColor = textureLod(tFx, vUV, 0.0); }
