#version 450
// Educational edge-adaptive spatial upscaler in the spirit of FSR 1 EASU (12-tap, direction and
// anisotropy-stretched Lanczos-2 kernel, ringing clamp). Simplified from the public description.
layout(location = 0) in vec2 vUV;              // OUTPUT-resolution uv
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uTex;    // render-resolution colour (tonemapped or HDR)
layout(std140, binding = 3) uniform EasuParams { vec4 uInputSize; /* xy size, zw 1/size */ vec4 uViewportUV; /* xy = render/allocated */ };
float lum(vec3 c) { return dot(c, vec3(0.5, 1.0, 0.5)); }
vec3 fetch(ivec2 p) { ivec2 lim = ivec2(uInputSize.xy) - 1; return texelFetch(uTex, clamp(p, ivec2(0), lim), 0).rgb; }
float lanczos2Approx(float x2) {          // x2 = squared distance, window to 2 texels
    float w = 0.4 * x2 - 1.0;             // (25/16 (2/5 x^2 - 1)^2 - (25/16 - 1)) * (1/4 x^2 - 1)^2 approximation
    float b = (1.0 / 4.0) * x2 - 1.0;
    return max((25.0 / 16.0) * w * w - (25.0 / 16.0 - 1.0), 0.0) * b * b;
}
void main() {
    vec2 pp = vUV * uInputSize.xy - 0.5;
    ivec2 fp = ivec2(floor(pp)); vec2 f = pp - vec2(fp);
    // 12-tap footprint:   b c
    //                   e f g h
    //                   i j k l
    //                     n o
    vec3 B = fetch(fp + ivec2(0, -1)), C = fetch(fp + ivec2(1, -1));
    vec3 E = fetch(fp + ivec2(-1, 0)), F = fetch(fp), G = fetch(fp + ivec2(1, 0)), H = fetch(fp + ivec2(2, 0));
    vec3 I = fetch(fp + ivec2(-1, 1)), J = fetch(fp + ivec2(0, 1)), K = fetch(fp + ivec2(1, 1)), Lc = fetch(fp + ivec2(2, 1));
    vec3 N = fetch(fp + ivec2(0, 2)), O = fetch(fp + ivec2(1, 2));
    // direction from the 2x2 centre's gradients
    float lF = lum(F), lG = lum(G), lJ = lum(J), lK = lum(K);
    vec2 dir = vec2((lG - lF) * (1.0 - f.y) + (lK - lJ) * f.y, (lJ - lF) * (1.0 - f.x) + (lK - lG) * f.x);
    float len = length(dir);
    dir = len > 1e-5 ? dir / len : vec2(1, 0);
    float stretch = clamp(len * 4.0, 0.0, 1.0);          // anisotropy: stretch kernel along the edge
    vec2 lenScale = vec2(1.0 + (sqrt(2.0) - 1.0) * stretch, 1.0 - 0.5 * stretch);
    vec3 taps[12] = vec3[](B, C, E, F, G, H, I, J, K, Lc, N, O);
    vec2 offs[12] = vec2[](vec2(0,-1), vec2(1,-1), vec2(-1,0), vec2(0,0), vec2(1,0), vec2(2,0),
                           vec2(-1,1), vec2(0,1), vec2(1,1), vec2(2,1), vec2(0,2), vec2(1,2));
    vec3 sum = vec3(0); float wsum = 0.0;
    for (int i = 0; i < 12; i++) {
        vec2 d = offs[i] - f;
        // rotate into edge space: x across the edge gradient, y along the edge
        vec2 r = vec2(dot(d, dir), dot(d, vec2(-dir.y, dir.x))) * vec2(lenScale.x, lenScale.y);
        float w = lanczos2Approx(min(dot(r, r), 4.0));
        sum += taps[i] * w; wsum += w;
    }
    vec3 res = sum / max(wsum, 1e-5);
    vec3 mn = min(min(F, G), min(J, K)), mx = max(max(F, G), max(J, K));   // anti-ringing
    oColor = vec4(clamp(res, mn, mx), 1.0);
}
