#version 450
#include "aa/common_temporal.glsl"
// Frame generation, step 1: refine game motion vectors with block-matching optical flow on luma.
// Runs at quarter resolution. Output: UV-space motion (current minus previous) + match confidence.
// Game MVs are exact for geometry; flow catches what MVs miss (shadows, reflections, scrolling textures).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oFlow;              // xy = uv delta (curr - prev), z = confidence, w = used MV(0)/flow(1)
layout(binding = 0) uniform sampler2D uLumaCurr;  // frame N final colour (luma computed here)
layout(binding = 1) uniform sampler2D uLumaPrev;  // frame N-1
layout(binding = 2) uniform sampler2D uVelocity;  // game motion vectors (NDC delta)
layout(std140, binding = 3) uniform FlowParams { vec4 uFlowSize; int uSearchRadius; float uLod; int uQuality; int _p0; };
float sad(vec2 uvC, vec2 uvP, vec2 px) {          // 3x3 block sum of absolute differences
    float s = 0.0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        vec2 o = vec2(x, y) * px;
        s += abs(luma(textureLod(uLumaCurr, uvC + o, uLod).rgb) - luma(textureLod(uLumaPrev, uvP + o, uLod).rgb));
    }
    return s;
}
void main() {
    vec2 px = uFlowSize.zw;
    vec2 mv = velocityToUV(textureLod(uVelocity, vUV, 0.0).rg);
    float bestS = sad(vUV, vUV - mv, px); vec2 best = mv;
    float mvS = bestS;
    // search around the MV guess (small window, since MVs are usually close)
    for (int y = -uSearchRadius; y <= uSearchRadius; y++) for (int x = -uSearchRadius; x <= uSearchRadius; x++) {
        if (x == 0 && y == 0) continue;
        vec2 cand = mv + vec2(x, y) * px;
        float s = sad(vUV, vUV - cand, px) + 0.002 * float(abs(x) + abs(y)); // prefer small corrections
        if (s < bestS) { bestS = s; best = cand; }
    }
    float conf = exp(-bestS * 4.0);
    // Only override game MVs where they clearly fail (MV match is poor AND the flow match is much
    // better): block matching is unreliable on flat or repetitive areas, where MVs are exact.
    bool useFlow = mvS > 0.15 && bestS < mvS * 0.5;
    oFlow = vec4(useFlow ? best : mv, conf, useFlow ? 1.0 : 0.0);
}
