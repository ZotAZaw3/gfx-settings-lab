#version 450
// Naive unsharp mask, for comparison with CAS (shows halos).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uTex;
layout(std140, binding = 3) uniform SharpenParams { float uSharpness; int uSplit; float uSplitX; float uRange; }; // uRange: 1 SDR, peak/paperwhite HDR
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), lim = textureSize(uTex, 0) - 1;
    vec3 c = texelFetch(uTex, p, 0).rgb / uRange;
    if (uSplit != 0 && vUV.x < uSplitX) { oColor = vec4(c * uRange, 1); return; }
    vec3 blur = vec3(0);
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++)
        blur += texelFetch(uTex, clamp(p + ivec2(x, y), ivec2(0), lim), 0).rgb / uRange * ((x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0) / 16.0);
    oColor = vec4(clamp(c + 1.5 * uSharpness * (c - blur), 0.0, 1.0) * uRange, 1.0);
}
