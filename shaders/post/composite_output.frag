#version 450
// PostOutput composite: scene + bloom (and lens dirt) + lens flare + god rays + sun glare starburst. Linear HDR.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uBloom;
layout(binding = 2) uniform sampler2D uBloomWide;
layout(binding = 3) uniform sampler2D uDirt;
layout(binding = 4) uniform sampler2D uFlare;
layout(binding = 5) uniform sampler2D uRays;
layout(binding = 6) uniform sampler2D uDepth;
float sunVisibility() {
    // fraction of 16 depth taps around the sun that see the sky
    if (pSun.z < 0.5) return 0.0;
    float v = 0.0;
    for (int i = 0; i < 16; i++) {
        float a = float(i) * 2.39996, r = 0.01 * sqrt(float(i) + 0.5);
        vec2 uv = pSun.xy + vec2(cos(a) / pSun.w, sin(a)) * r;
        if (any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1)))) continue;
        v += textureLod(uDepth, uv, 0.0).r <= 1e-7 ? 1.0 : 0.0;
    }
    float edge = smoothstep(-0.2, 0.05, min(min(pSun.x, 1.0 - pSun.x), min(pSun.y, 1.0 - pSun.y)));
    return v / 16.0 * edge;
}
void main() {
    int debugView = int(pLetterbox.z);
    vec3 c = textureLod(uSrc, vUV, 0.0).rgb;
    float dirt = textureLod(uDirt, vUV, 0.0).r;
    if (pBloom2.w > 0.5) {
        vec3 b = textureLod(uBloom, vUV, 0.0).rgb * pBloomTint.rgb;
        vec3 bw = textureLod(uBloomWide, vUV, 0.0).rgb * pBloomTint.rgb;
        c += b * pBloom2.x + bw * dirt * pBloom2.z;
        if (debugView == 1) { oColor = vec4(b * pBloom2.x * 10.0, 1); return; }
    }
    if (pFlare2.y > 0.5) {
        vec3 fl = textureLod(uFlare, vUV, 0.0).rgb * pFlare.x;
        c += fl * (0.6 + dirt);
        if (debugView == 2) { oColor = vec4(fl * 4.0, 1); return; }
    }
    if (pRays2.z > 0.5) {
        vec3 r = textureLod(uRays, vUV, 0.0).rgb * pRays2.x;
        c += r;
        if (debugView == 3) { oColor = vec4(r, 1); return; }
    }
    if (pGlare2.x > 0.5) {
        float vis = sunVisibility();
        if (vis > 0.0) {
            vec2 d = (vUV - pSun.xy) * vec2(pSun.w, 1.0);
            float r = length(d) / pGlare.z, a = atan(d.y, d.x);
            float glow = exp(-r * 4.0) * 0.6 + exp(-r * 20.0);
            float star = pow(abs(cos(a * pGlare.y * 0.5)), 60.0) * exp(-r * 2.5);
            float ana = exp(-abs(d.y) * 220.0) * exp(-abs(d.x) * 1.5) * pGlare.w;
            vec3 sun = pSunRadiance.rgb;
            c += sun * vis * pGlare.x * (glow + star * 0.7) + sun * vis * ana * vec3(0.4, 0.6, 1.0);
        }
    }
    oColor = vec4(c, 1);
}
