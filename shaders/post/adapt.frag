#version 450
// PostOutput eye adaptation 2/2: temporal adaptation of the exposure EV in a 1x1 R32F target.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uLum;    // mip chain of (log*w, w)
layout(binding = 1) uniform sampler2D uPrev;   // previous EV
void main() {
    vec2 s = textureLod(uLum, vec2(0.5), pExposure3.x).rg;
    float avgLog2 = s.x / max(s.y, 1e-5);
    float target = clamp(log2(pExposure.w) - avgLog2, pExposure.y, pExposure.z);   // exposure = key / avgLum
    float cur = textureLod(uPrev, vec2(0.5), 0.0).r;
    if (pExposure3.y > 0.5 || isnan(cur) || isinf(cur)) cur = target;
    float speed = target < cur ? pExposure2.x : pExposure2.y;   // EV going down = scene got brighter
    oColor = vec4(cur + (target - cur) * (1.0 - exp(-frameDt() * speed)), 0, 0, 1);
}
