#version 450
// AMD FidelityFX CAS (5-tap variant), rewritten from the public algorithm. LDR input.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uTex;
layout(std140, binding = 3) uniform SharpenParams { float uSharpness; int uSplit; float uSplitX; float uRange; }; // uRange: 1 SDR, peak/paperwhite HDR
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), lim = textureSize(uTex, 0) - 1;
    vec3 c = texelFetch(uTex, p, 0).rgb / uRange;
    if (uSplit != 0 && vUV.x < uSplitX) { oColor = vec4(c * uRange, 1); return; }
    vec3 a = texelFetch(uTex, clamp(p + ivec2(0, -1), ivec2(0), lim), 0).rgb / uRange;
    vec3 b = texelFetch(uTex, clamp(p + ivec2(-1, 0), ivec2(0), lim), 0).rgb / uRange;
    vec3 d = texelFetch(uTex, clamp(p + ivec2(1, 0),  ivec2(0), lim), 0).rgb / uRange;
    vec3 e = texelFetch(uTex, clamp(p + ivec2(0, 1),  ivec2(0), lim), 0).rgb / uRange;
    vec3 mn = min(min(min(a, b), min(c, d)), e);
    vec3 mx = max(max(max(a, b), max(c, d)), e);
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));
    vec3 w = amp * mix(-0.125, -0.2, uSharpness);
    oColor = vec4(clamp((c + w * (a + b + d + e)) / (1.0 + 4.0 * w), 0.0, 1.0) * uRange, 1.0);
}
