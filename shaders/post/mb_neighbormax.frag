#version 450
// PostRender motion blur 2/3: max velocity over the 3x3 neighbouring tiles.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uTiles;
void main() {
    ivec2 ts = textureSize(uTiles, 0), p = ivec2(gl_FragCoord.xy);
    vec2 best = vec2(0); float bl = 0.0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        vec2 v = texelFetch(uTiles, clamp(p + ivec2(x, y), ivec2(0), ts - 1), 0).xy;
        float l = dot(v, v);
        if (l > bl) { bl = l; best = v; }
    }
    oColor = vec4(best, 0, 1);
}
