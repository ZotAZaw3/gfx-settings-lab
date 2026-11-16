#version 450
// PostLDR uber pass: chromatic aberration -> color grading LUT -> vignette -> film grain -> letterbox.
// Input/output: display-referred linear. The core applies gamma afterwards.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler3D uLUT;

vec3 chromaticAberration(vec2 uv) {   // spectral taps along the radial direction
    vec2 d = uv - 0.5;
    vec2 off = d * dot(d, d) * pCA.x * 0.1;
    int N = max(int(pCA.y), 3);
    vec3 sum = vec3(0), wsum = vec3(0);
    for (int i = 0; i < N; i++) {
        float t = float(i) / float(N - 1);            // 0 red end .. 1 blue end
        vec3 w = clamp(vec3(1.0 - abs(t * 2.0), 1.0 - abs(t * 2.0 - 1.0), 1.0 - abs(t * 2.0 - 2.0)), 0.0, 1.0);
        sum += textureLod(uSrc, uv + off * (0.5 - t) * 2.0, 0.0).rgb * w;
        wsum += w;
    }
    return sum / wsum;
}
vec3 applyLUT(vec3 c) {
    float N = pLetterbox.w;
    vec3 g = textureLod(uLUT, clamp(c, 0.0, 1.0) * ((N - 1.0) / N) + 0.5 / N, 0.0).rgb;
    return mix(c, g, pGrade2.z);
}
vec3 applyVignette(vec3 c, vec2 uv) {
    float aspect = uOutputSize.x * uOutputSize.w;
    vec2 d = uv - 0.5;
    d.x *= mix(1.0, aspect, pVignette.z);
    float r = length(d) * 1.41421 / mix(1.0, aspect, pVignette.z * 0.5);
    float v = smoothstep(1.0 - pVignette.y - pVignette.x * 0.4, 1.0 + pVignette.y * 0.5, r);
    return mix(c, pVignetteColor.rgb, clamp(v * pVignette.x, 0.0, 1.0));
}
vec3 applyGrain(vec3 c) {
    uvec2 p = uvec2(floor(gl_FragCoord.xy / max(pGrain.y, 1.0)));
    float amt = pGrain.x * mix(1.0, 1.0 - sqrt(clamp(luma(c), 0.0, 1.0)), pGrain.z);   // less grain in highlights
    if (pGrain2.x > 0.5) return c + (hash33(uvec3(p, frameIndex())) - 0.5) * amt;
    return c + (hash13(uvec3(p, frameIndex())) - 0.5) * amt;
}
void main() {
    vec2 uv = vUV;
    vec3 c = pCA.z > 0.5 ? chromaticAberration(uv) : textureLod(uSrc, uv, 0.0).rgb;
    if (pGrade2.w > 0.5 && pTonemap.z < 0.5) c = applyLUT(c);     // LDR LUT only applies to SDR output
    if (pVignette.w > 0.5) c = applyVignette(c, uv);
    if (pGrain.w > 0.5) c = applyGrain(c);
    else c += (hash13(uvec3(uvec2(gl_FragCoord.xy), frameIndex())) - 0.5) / 255.0;   // tiny dither vs banding
    float bar = pLetterbox.x;
    if (bar > 0.0) {
        float px = uOutputSize.w;
        float m = smoothstep(bar - px, bar, uv.y) * smoothstep(bar - px, bar, 1.0 - uv.y);
        c = mix(pLetterboxColor.rgb, c, m);
    }
    if (pLetterbox.y > 0.5) {   // debug: LUT slices as a strip in the top-left corner, 4x zoom
        float N = pLetterbox.w;
        vec2 sp = gl_FragCoord.xy / 4.0;
        if (sp.y < N && sp.x < N * N) c = textureLod(uLUT, (vec3(mod(sp.x, N), sp.y, floor(sp.x / N)) + 0.5) / N, 0.0).rgb;
    }
    if (pCalib.z > 0.5) {       // HDR calibration test pattern replaces the image (values relative to paper white)
        float paper = pDisplay.x;
        vec2 p = (uv - 0.5) * vec2(uOutputSize.x * uOutputSize.w, 1.0);
        float logo = step(max(abs(p.x), abs(p.y)), 0.12) * (1.0 - step(length(p), 0.05));
        int step_ = int(pCalib.x); float nits = pCalib.y;
        if (step_ == 0)      c = vec3(mix(10000.0, nits, logo));                                   // logo vanishes when it clips
        else if (step_ == 1) c = vec3(mix(0.18 * paper, paper, step(uv.x, 0.5)));                  // grey card vs paper white
        else                 c = vec3(mix(0.0, nits, logo));                                       // barely visible logo on black
        c /= paper;
        if (pTonemap.z < 0.5) c = clamp(c, 0.0, 1.0);
    }
    oColor = vec4(max(c, vec3(0)), 1);
}
