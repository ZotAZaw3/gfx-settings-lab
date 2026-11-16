#version 450
// Cloth surface: vertex pulling from the simulated positions; normal from grid neighbours.
// cA.z > 0.5 -> cloth sim Off: positions come from the rest pose + a cheap analytic wave ("baked animation").
#include "geometry/geo_common.glsl"
#include "geometry/char_params.glsl"
layout(std430, binding = 8) readonly buffer P { vec4 pos[]; };
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec4 vCurClip;
layout(location = 3) out vec4 vPrevClip;
vec3 P_(int x, int y){
    int W = int(cA.x), H = int(cA.y);
    x = clamp(x, 0, W - 1); y = clamp(y, 0, H - 1);
    vec3 p = pos[y * W + x].xyz;
    if (cA.z > 0.5){
        float u = float(x) / float(W - 1);
        p.z += sin(uTime.x * 4.0 - u * 6.0) * 0.25 * u;          // analytic flutter
    }
    return p;
}
void main(){
    // non-indexed: 6 vertices per grid quad (vertex pulling, no index buffer)
    int W = int(cA.x);
    int quad = gl_VertexID / 6, corner = gl_VertexID % 6;
    const ivec2 off[6] = ivec2[](ivec2(0,0), ivec2(0,1), ivec2(1,0), ivec2(1,0), ivec2(0,1), ivec2(1,1));
    int x = quad % (W - 1) + off[corner].x, y = quad / (W - 1) + off[corner].y;
    vec3 p = P_(x, y);
    vec3 dx = P_(x + 1, y) - P_(x - 1, y), dy = P_(x, y + 1) - P_(x, y - 1);
    vNormal = normalize(cross(dx, dy));
    float stripe = step(0.5, fract(float(y) / float(int(cA.y) - 1) * 3.0));
    vColor = mix(cB.rgb, vec3(0.9), stripe);
    gl_Position = uViewProj * vec4(p, 1.0);
    vCurClip = uUnjitteredViewProj * vec4(p, 1.0);
    vPrevClip = uPrevViewProj * vec4(p, 1.0);
}
