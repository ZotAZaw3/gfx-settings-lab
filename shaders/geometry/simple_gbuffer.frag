#version 450
// Shared G-buffer fragment shader for the simple character meshes (crowd, cloth, fur base).
#include "geometry/geo_common.glsl"
#include "geometry/char_params.glsl"
#include "geometry/geo_gbuffer.glsl"
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec4 vCurClip;
layout(location = 3) in vec4 vPrevClip;
void main(){
    vec3 N = gl_FrontFacing ? vNormal : -vNormal;
    writeGBuffer(vColor, N, cB.w, 0.0, 1.0, vec3(0.0), GEO_SHADING_DEFAULT, vCurClip, vPrevClip);
}
