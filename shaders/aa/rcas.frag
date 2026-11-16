#version 450
// RCAS-style robust sharpening (FSR's sharpener), simplified: limits the negative lobe so the
// result never leaves the neighbourhood min/max, plus a noise-detection term.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uTex;
layout(std140, binding = 3) uniform SharpenParams { float uSharpness; int uSplit; float uSplitX; float uRange; }; // uRange: 1 SDR, peak/paperwhite HDR
float lum(vec3 c) { return dot(c, vec3(0.5, 1.0, 0.5)); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), lim = textureSize(uTex, 0) - 1;
    vec3 e = texelFetch(uTex, p, 0).rgb / uRange;
    if (uSplit != 0 && vUV.x < uSplitX) { oColor = vec4(e * uRange, 1); return; }
    vec3 b = texelFetch(uTex, clamp(p + ivec2(0, -1), ivec2(0), lim), 0).rgb / uRange;
    vec3 d = texelFetch(uTex, clamp(p + ivec2(-1, 0), ivec2(0), lim), 0).rgb / uRange;
    vec3 f = texelFetch(uTex, clamp(p + ivec2(1, 0),  ivec2(0), lim), 0).rgb / uRange;
    vec3 h = texelFetch(uTex, clamp(p + ivec2(0, 1),  ivec2(0), lim), 0).rgb / uRange;
    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = mn4 / (4.0 * mx4 + 1e-5);
    vec3 hitMax = (1.0 - mx4) / (4.0 * mn4 - 4.0 - 1e-5);
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * exp2(-(1.0 - uSharpness) * 2.0);
    // noise detection: reduce sharpening where the centre is an isolated outlier
    float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float range = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz = clamp(abs(nz) / max(range, 1e-5), 0.0, 1.0);
    lobe *= 1.0 - 0.5 * nz;
    oColor = vec4(clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0) * uRange, 1.0);
}
