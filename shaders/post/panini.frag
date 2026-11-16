#version 450
// PostOutput: Panini projection remap for wide FOV. See docs/panini_projection.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
vec2 paniniInverse(vec2 p, float d) {   // panini plane -> rectilinear view plane
    float k = p.x * p.x / ((d + 1.0) * (d + 1.0));
    float dscr = max(k * k * d * d - (k + 1.0) * (k * d * d - 1.0), 0.0);
    float clon = (-k * d + sqrt(dscr)) / (k + 1.0);
    float S = (d + 1.0) / (d + clon);
    float lon = atan(p.x, S * clon);
    float lat = atan(p.y, S);
    return vec2(sin(lon), tan(lat)) / cos(lon);
}
void main() {
    float d = pPanini.x, crop = pPanini.y; vec2 he = pPanini.zw;
    float lonE = atan(he.x);
    float edgeX = (d + 1.0) / (d + cos(lonE)) * sin(lonE);          // panini image of the frame's side edge
    float f = (d + 1.0) * cos(lonE) / (d + cos(lonE));              // vertical shrink at the sides
    vec2 p = uvToNdc(vUV) * vec2(edgeX, he.y) * mix(1.0, f, crop);  // zoom in to hide empty corners
    vec2 r = paniniInverse(p, d);
    vec2 uv = ndcToUv(r / he);
    if (any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1)))) { oColor = vec4(0, 0, 0, 1); return; }
    oColor = vec4(textureLod(uSrc, uv, 0.0).rgb, 1);
}
