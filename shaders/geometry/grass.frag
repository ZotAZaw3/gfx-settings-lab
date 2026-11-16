#version 450
#include "geometry/geo_common.glsl"
#include "geometry/geo_gbuffer.glsl"
layout(location = 0) in vec3 vNormal;
layout(location = 1) in float vT;
layout(location = 2) in float vVar;
layout(location = 3) in vec4 vCurClip;
layout(location = 4) in vec4 vPrevClip;
void main(){
    vec3 base = mix(vec3(0.10, 0.20, 0.04), vec3(0.20, 0.34, 0.07), vVar);
    vec3 col = mix(base * 0.45, base * 1.15 + vec3(0.04, 0.05, 0.0), vT);   // darker at the root (fake AO)
    vec3 N = gl_FrontFacing ? vNormal : -vNormal;
    N = normalize(mix(N, vec3(0.0, 1.0, 0.0), 0.5));                         // soften: grass reads better lit from above
    writeGBuffer(col, N, 0.8, 0.0, mix(0.4, 1.0, vT), vec3(0.0), GEO_SHADING_FOLIAGE, vCurClip, vPrevClip);
}
