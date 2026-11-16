#version 450
// Composite (possibly low-res) fog onto scene.color with a depth-aware 2x2 upsample.
// No blending: reads a copy of scene.color and writes scene * T + inscatter (rhi has no ONE/SRC_ALPHA blend mode).
#include "common/frame.glsl"
layout(binding = 0) uniform sampler2D tFog;          // rgb inscatter, a transmittance
layout(binding = 1) uniform sampler2D tSceneDepth;   // full res
layout(binding = 2) uniform sampler2D tFogDepth;     // depth the fog was computed with
layout(binding = 3) uniform sampler2D tSceneColor;   // copy of scene.color (we write the result, no blending)
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main(){
    float d = linearDepth(textureLod(tSceneDepth, vUV, 0.0).r);
    vec2 lowSize = vec2(textureSize(tFog, 0)), tc = vUV * lowSize - 0.5, f = fract(tc);
    vec2 base = (floor(tc) + 0.5) / lowSize;
    vec4 acc = vec4(0.0); float wsum = 0.0;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x){
        vec2 uv = base + vec2(x, y) / lowSize;
        float bw = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
        float dd = abs(linearDepth(textureLod(tFogDepth, uv, 0.0).r) - d) / max(d, 1e-3);
        float w = bw / (dd * 20.0 + 1e-3);
        acc += textureLod(tFog, uv, 0.0) * w; wsum += w;
    }
    vec4 fog = acc / max(wsum, 1e-6);
    vec3 scene = textureLod(tSceneColor, vUV, 0.0).rgb;
    outColor = vec4(scene * fog.a + fog.rgb, 1.0);
}
