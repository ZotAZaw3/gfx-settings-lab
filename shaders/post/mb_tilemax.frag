#version 450
// PostRender motion blur 1/3: dominant velocity (pixels) per 16x16 tile. See docs/motion_blur.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uVelocity;   // NDC(cur) - NDC(prev)
const int TILE = 16;
void main() {
    ivec2 vs = textureSize(uVelocity, 0);
    ivec2 base = ivec2(gl_FragCoord.xy) * TILE;
    vec2 best = vec2(0); float bl = 0.0;
    for (int y = 0; y < TILE; y++) for (int x = 0; x < TILE; x++) {
        vec2 v = texelFetch(uVelocity, min(base + ivec2(x, y), vs - 1), 0).xy * vec2(0.5, -0.5) * vec2(vs) * pMB.x;
        float l = dot(v, v);
        if (l > bl) { bl = l; best = v; }
    }
    float len = length(best);
    if (len > pMB.y) best *= pMB.y / len;
    oColor = vec4(best, 0, 1);
}
