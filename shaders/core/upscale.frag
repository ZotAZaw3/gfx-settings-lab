#version 450
// Spatial upscale for render scale (FILTER 0 nearest, 1 bilinear, 2 Catmull-Rom bicubic, 9 taps).
#include "common/frame.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D tSrc;
layout(std140, binding = 2) uniform CoreParams { vec4 uSrcSize; vec4 uP1; ivec4 uI0; };

vec4 catmullRom(vec2 uv) {
    // 9-tap Catmull-Rom using bilinear hardware filtering (Jimenez / MJP approach)
    vec2 pos = uv * uSrcSize.xy;
    vec2 tc1 = floor(pos - 0.5) + 0.5;
    vec2 f = pos - tc1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc0 = (tc1 - 1.0) * uSrcSize.zw;
    vec2 tc3 = (tc1 + 2.0) * uSrcSize.zw;
    vec2 tc12 = (tc1 + w2 / w12) * uSrcSize.zw;
    vec4 r = vec4(0.0);
    r += textureLod(tSrc, vec2(tc0.x, tc0.y), 0) * w0.x * w0.y;
    r += textureLod(tSrc, vec2(tc12.x, tc0.y), 0) * w12.x * w0.y;
    r += textureLod(tSrc, vec2(tc3.x, tc0.y), 0) * w3.x * w0.y;
    r += textureLod(tSrc, vec2(tc0.x, tc12.y), 0) * w0.x * w12.y;
    r += textureLod(tSrc, vec2(tc12.x, tc12.y), 0) * w12.x * w12.y;
    r += textureLod(tSrc, vec2(tc3.x, tc12.y), 0) * w3.x * w12.y;
    r += textureLod(tSrc, vec2(tc0.x, tc3.y), 0) * w0.x * w3.y;
    r += textureLod(tSrc, vec2(tc12.x, tc3.y), 0) * w12.x * w3.y;
    r += textureLod(tSrc, vec2(tc3.x, tc3.y), 0) * w3.x * w3.y;
    return max(r, vec4(0.0));
}

void main() {
#if FILTER == 2
    oColor = catmullRom(vUV);
#else
    oColor = textureLod(tSrc, vUV, 0);
#endif
}
