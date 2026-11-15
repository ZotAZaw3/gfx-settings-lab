#version 450
// Output transform: brightness, gamma (SDR) or PQ / scRGB encoding (HDR), plus buffer debug views.
#include "common/frame.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D tSrc;        // display-referred linear (tone mapped)
layout(binding = 1) uniform sampler2D tAlbedo;
layout(binding = 2) uniform sampler2D tNormal;
layout(binding = 3) uniform sampler2D tMaterial;
layout(binding = 4) uniform sampler2D tDepth;
layout(binding = 5) uniform sampler2D tVelocity;
layout(binding = 6) uniform sampler2D tEmissive;
layout(binding = 7) uniform sampler2D tSceneHdr;
layout(std140, binding = 2) uniform CoreParams { vec4 uP0; vec4 uP1; ivec4 uI0; };   // x debug view, y HDR swapchain active

vec3 pqEncode(vec3 nits) {   // SMPTE ST 2084
    vec3 y = clamp(nits / 10000.0, 0.0, 1.0);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 ym = pow(y, vec3(m1));
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}
const mat3 REC709_TO_REC2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);

void main() {
    int view = uI0.x;
    vec3 c;
    if (view == 0) c = textureLod(tSrc, vUV, 0).rgb;
    else if (view == 1) c = texture(tAlbedo, vUV).rgb;
    else if (view == 2) c = texture(tNormal, vUV).xyz * 0.5 + 0.5;
    else if (view == 3) c = vec3(texture(tMaterial, vUV).r);
    else if (view == 4) c = vec3(texture(tMaterial, vUV).g);
    else if (view == 5) c = vec3(texture(tMaterial, vUV).b);
    else if (view == 6) { float d = texture(tDepth, vUV).r; c = vec3(clamp(1.0 - log2(linearDepth(d) + 1.0) / 8.0, 0.0, 1.0)); }
    else if (view == 7) { vec2 v = texture(tVelocity, vUV).xy * 20.0; c = vec3(abs(v), 0.0); }
    else if (view == 8) c = texture(tEmissive, vUV).rgb;
    else c = texture(tSceneHdr, vUV).rgb / (1.0 + texture(tSceneHdr, vUV).rgb);
    c = max(c, 0.0) * uDisplay.z;   // brightness

    int mode = int(uDisplay2.y);
    float paperWhite = uDisplay.w, peak = uDisplay2.x, black = uDisplay2.w;
    if (mode == 1) {
        // HDR10: scene value 1.0 = paper white nits, Rec.709 -> Rec.2020, PQ curve.
        vec3 nits = REC709_TO_REC2020 * c * paperWhite;
        nits = min(nits, vec3(peak)) + black;
        c = pqEncode(nits);
        // No HDR swapchain (GL): show the PQ signal as-is so the encoding is visible (washed out on SDR).
    } else if (mode == 2) {
        // scRGB: linear, 1.0 = 80 nits. Without an FP16 HDR swapchain this preview is clipped.
        c = c * (paperWhite / 80.0);
        if (uI0.y == 0) c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / uDisplay.y));
    } else {
        c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / uDisplay.y));   // gamma encode for an SDR display
    }
    oColor = vec4(c, 1.0);
}
