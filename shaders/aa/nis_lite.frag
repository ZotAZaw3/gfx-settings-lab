#version 450
// "NIS-lite": educational directional scaler + sharpener inspired by NVIDIA Image Scaling.
// Not the coefficient-table NIS shader; picks one of 4 edge orientations and filters along it.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uTex;   // input at render resolution, linear clamp
layout(std140, binding = 3) uniform SharpenParams { float uSharpness; int uSplit; float uSplitX; float uRange; }; // uRange: 1 SDR, peak/paperwhite HDR
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec2 px = 1.0 / vec2(textureSize(uTex, 0));
    vec3 c = textureLod(uTex, vUV, 0.0).rgb;
    if (uSplit != 0 && vUV.x < uSplitX) { oColor = vec4(c, 1); return; }
    vec2 dirs[4] = vec2[](vec2(1, 0), vec2(0, 1), vec2(0.7071, 0.7071), vec2(0.7071, -0.7071));
    // gradient energy across each direction: the edge runs along the direction with the lowest
    float best = 1e9; int bi = 0;
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2(-dirs[i].y, dirs[i].x) * px;
        float g = abs(lum(textureLod(uTex, vUV + dirs[i] * px, 0.0).rgb) - lum(textureLod(uTex, vUV - dirs[i] * px, 0.0).rgb));
        if (g < best) { best = g; bi = i; }
    }
    vec2 along = dirs[bi] * px, across = vec2(-dirs[bi].y, dirs[bi].x) * px;
    // smooth along the edge (removes stair-steps), sharpen across it
    vec3 sAlong = (textureLod(uTex, vUV + along, 0.0).rgb + textureLod(uTex, vUV - along, 0.0).rgb) * 0.25 + c * 0.5;
    vec3 a1 = textureLod(uTex, vUV + across, 0.0).rgb, a2 = textureLod(uTex, vUV - across, 0.0).rgb;
    vec3 sharp = sAlong + uSharpness * (sAlong - 0.5 * (a1 + a2));
    vec3 mn = min(c, min(a1, a2)), mx = max(c, max(a1, a2));
    oColor = vec4(clamp(sharp, mn, mx), 1.0);
}
