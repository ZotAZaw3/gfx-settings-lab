#version 450
// PostRender motion blur 3/3: reconstruction filter (McGuire et al. 2012, simplified).
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uVelocity;
layout(binding = 2) uniform sampler2D uNeighborMax;
layout(binding = 3) uniform sampler2D uDepth;
vec2 pixelVelocity(vec2 uv, vec2 size) {
    vec2 v;
    if (pMB.w > 0.5) {   // camera only: reproject depth with last frame's matrix
        float d = textureLod(uDepth, uv, 0.0).r;
        vec4 ndc = vec4(uvToNdc(uv), d, 1.0);
        vec4 ws = uInvViewProj * ndc; ws /= ws.w;   // jitter is sub-pixel, fine for blur
        vec4 prev = uPrevViewProj * ws;
        v = ndc.xy - prev.xy / prev.w;
    } else {
        v = textureLod(uVelocity, uv, 0.0).xy;
    }
    return v * vec2(0.5, -0.5) * size * pMB.x;
}
void main() {
    vec2 size = uRenderSize.xy;
    int N = int(pMB.z);
    vec3 c0 = textureLod(uSrc, vUV, 0.0).rgb;
    vec2 vn = pMB.w > 0.5 ? pixelVelocity(vUV, size) : textureLod(uNeighborMax, vUV, 0.0).xy;
    float ln = length(vn);
    if (ln < 0.5) { oColor = vec4(c0, 1); return; }   // nothing moves here
    if (ln > pMB.y) { vn *= pMB.y / ln; ln = pMB.y; }
    float l0 = max(length(pixelVelocity(vUV, size)), 0.5);
    float z0 = linearDepth(textureLod(uDepth, vUV, 0.0).r);
    float j = hash13(uvec3(uvec2(gl_FragCoord.xy), frameIndex())) - 0.5;
    vec3 acc = c0 / l0; float wsum = 1.0 / l0;
    for (int i = 0; i < N; i++) {
        float t = mix(-0.5, 0.5, (float(i) + 0.5 + j) / float(N));
        vec2 uv = vUV + vn * t / size;
        float dist = abs(t) * ln;
        float ls = max(length(pixelVelocity(uv, size)), 0.5);
        float zs = linearDepth(textureLod(uDepth, uv, 0.0).r);
        float soft = max(z0 * 0.05, 1e-3);
        float fg = clamp(1.0 - (zs - z0) / soft, 0.0, 1.0);   // sample in front of the centre pixel
        float bg = clamp(1.0 + (zs - z0) / soft, 0.0, 1.0);   // sample behind it
        float coneS = clamp(1.0 - dist / ls, 0.0, 1.0), cone0 = clamp(1.0 - dist / l0, 0.0, 1.0);
        float w = fg * coneS + bg * cone0 + 2.0 * min(coneS, cone0);
        acc += textureLod(uSrc, uv, 0.0).rgb * w; wsum += w;
    }
    oColor = vec4(acc / wsum, 1);
}
