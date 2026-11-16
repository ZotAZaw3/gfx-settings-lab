#version 450
// Tessellation evaluation (domain) shader: place generated vertex, sample height, displace.
#include "geometry/geo_common.glsl"
#include "geometry/terrain_params.glsl"
layout(quads, fractional_odd_spacing, ccw) in;
layout(location = 0) in vec3 tcPos[];
layout(location = 0) out vec3 vPos;
layout(location = 1) out vec4 vCurClip;
layout(location = 2) out vec4 vPrevClip;
void main(){
    vec2 t = gl_TessCoord.xy;
    vec3 p = mix(mix(tcPos[0], tcPos[1], t.x), mix(tcPos[3], tcPos[2], t.x), t.y);
    p.y = terrainHeight(terrainUV(p));
    vPos = p;
    gl_Position = uViewProj * vec4(p, 1.0);
    vCurClip = uUnjitteredViewProj * vec4(p, 1.0);
    vPrevClip = uPrevViewProj * vec4(p, 1.0);
}
