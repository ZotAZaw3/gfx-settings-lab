#version 450
#include "aa/common_temporal.glsl"
// Frame generation, step 2: synthesise the frame at time uT (0.5 for 2x) between N-1 and N.
// Gather formulation: for each output pixel, find the motion of the surface that will be here at
// time uT (candidate search + consistency test on frame N's flow), then read both source frames
// along it. Before UI (UI is composited afterwards by the core).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uFramePrev;   // N-1 final colour (LDR, pre-UI)
layout(binding = 1) uniform sampler2D uFrameCurr;   // N
layout(binding = 2) uniform sampler2D uFlow;        // from fg_optical_flow (UV delta curr - prev). POINT sampler:
                                                     // a bilinear blend of two motions is a motion nothing has.
layout(binding = 3) uniform sampler2D uDepthPrev;   // reversed-Z
layout(binding = 4) uniform sampler2D uDepthCurr;
layout(std140, binding = 3) uniform InterpParams { float uT; float uDepthSharpness; int uDebugView; int uFlowSearchIters; };
// Candidate motions are gathered from a neighbourhood of frame N's flow. A candidate m is
// "consistent" if the surface it lands on in frame N really moves by m: flow(uv + (1-t) m) ~= m.
// That finds the leading edge of a moving object (whose own pixel at uv has background motion).
void main() {
    vec2 fpx = 1.0 / vec2(textureSize(uFlow, 0));
    vec2 best = texture(uFlow, vUV).xy; float bestErr = 1e9;
    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {
        vec2 m = texture(uFlow, vUV + vec2(x, y) * fpx * 2.0).xy;
        vec2 land = texture(uFlow, vUV + (1.0 - uT) * m).xy;
        float err = length((land - m) / fpx) + 0.05 * float(abs(x) + abs(y));   // prefer the centre on ties
        if (err < bestErr) { bestErr = err; best = m; }
    }
    vec2 m = best;
    vec2 uvPrev = vUV - uT * m;            // where this surface was in N-1
    vec2 uvCurr = vUV + (1.0 - uT) * m;    // where it is in N (validated by the consistency test)
    vec3 a = texture(uFramePrev, uvPrev).rgb;
    vec3 b = texture(uFrameCurr, uvCurr).rgb;
    float za = texture(uDepthPrev, uvPrev).r, zb = texture(uDepthCurr, uvCurr).r;
    // same surface in both frames -> time-weighted blend; different depth -> it was hidden in N-1
    // (disocclusion), so only frame N's sample is right.
    float dz = abs(zb - za) / max(max(za, zb), 1e-6);
    float w = mix(uT, 1.0, clamp(dz * uDepthSharpness, 0.0, 1.0));
    if (outsideUV(uvPrev)) w = 1.0;
    if (outsideUV(uvCurr)) w = 0.0;
    vec3 res = mix(a, b, w);
    if (uDebugView == 1) res = vec3(w, 0.0, 1.0 - w);                  // which frame each pixel came from
    else if (uDebugView == 2) res = vec3(abs(m) * 40.0, texture(uFlow, vUV).w);
    oColor = vec4(res, 1.0);
}
