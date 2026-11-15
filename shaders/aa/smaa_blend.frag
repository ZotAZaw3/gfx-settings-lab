#version 450
// SMAA pass 3: neighbourhood blending. Each pixel gathers the weights that concern it:
//  own top edge (x: take from above), own left edge (z: take from left),
//  the top edge of the pixel below (its y: we take from below),
//  the left edge of the pixel to the right (its w: we take from the right).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uColor;
layout(binding = 1) uniform sampler2D uWeights;
layout(std140, binding = 3) uniform SmaaParams { vec4 uRcpSize; /* xy 1/size, z range (1 SDR) */ float uThreshold; int uMaxSearch; int uDiag; float uContrastFactor; };
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), lim = textureSize(uColor, 0) - 1;
    vec4 wSelf  = texelFetch(uWeights, p, 0);
    vec4 wBelow = texelFetch(uWeights, clamp(p + ivec2(0, 1), ivec2(0), lim), 0);
    vec4 wRight = texelFetch(uWeights, clamp(p + ivec2(1, 0), ivec2(0), lim), 0);
    float top = wSelf.x, left = wSelf.z, bottom = wBelow.y, right = wRight.w;
    vec3 c = texelFetch(uColor, p, 0).rgb;
    float total = top + left + bottom + right;
    if (total < 1e-4) { oColor = vec4(c, 1); return; }
    vec3 nb = texelFetch(uColor, clamp(p + ivec2(0, -1), ivec2(0), lim), 0).rgb * top
            + texelFetch(uColor, clamp(p + ivec2(-1, 0), ivec2(0), lim), 0).rgb * left
            + texelFetch(uColor, clamp(p + ivec2(0, 1), ivec2(0), lim), 0).rgb * bottom
            + texelFetch(uColor, clamp(p + ivec2(1, 0), ivec2(0), lim), 0).rgb * right;
    oColor = vec4(total <= 1.0 ? c * (1.0 - total) + nb : nb / total, 1.0);
}
