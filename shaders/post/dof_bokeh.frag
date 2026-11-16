#version 450
// PostRender DoF 2/4: scatter-as-gather bokeh disc at half res (golden-angle spiral).
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uHalf;   // rgb + signed CoC
void main() {
    float maxCoC = pDof.w, boostK = pDof2.z; int N = int(pDof2.w);
    vec2 t = 1.0 / vec2(textureSize(uHalf, 0));
    vec4 center = textureLod(uHalf, vUV, 0.0);
    vec3 accF = vec3(0), accN = vec3(0); float wF = 0.0, wN = 0.0;
    const float GA = 2.39996323;
    for (int i = 0; i < N; i++) {
        float r = sqrt((float(i) + 0.5) / float(N)) * maxCoC;
        float a = float(i) * GA;
        vec4 s = textureLod(uHalf, vUV + vec2(cos(a), sin(a)) * r * t, 0.0);
        float boost = 1.0 + boostK * max(luma(s.rgb) - 1.0, 0.0);   // highlights -> crisp discs
        float covF = smoothstep(r - 1.0, r + 1.0, max(s.a, 0.0)) * step(center.a - 1.0, s.a);
        float covN = smoothstep(r - 1.0, r + 1.0, max(-s.a, 0.0));
        accF += s.rgb * covF * boost; wF += covF * boost;
        accN += s.rgb * covN * boost; wN += covN * boost;
    }
    vec3 far = wF > 1e-4 ? accF / wF : center.rgb;
    float nearAlpha = clamp(wN / float(N) * 3.0, 0.0, 1.0);
    vec3 near = wN > 1e-4 ? accN / wN : far;
    float blend = max(smoothstep(0.5, 2.0, abs(center.a)), nearAlpha);
    oColor = vec4(mix(far, near, nearAlpha), blend);
}
