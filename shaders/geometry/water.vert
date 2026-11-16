#version 450
#include "geometry/geo_common.glsl"
#include "geometry/water_common.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vCrest;
void main(){
    vec3 p = vec3(wC.x, wB.y, wC.y) + aPos;
    vec3 N;
    vec3 d = waveSum(p.xz, uTime.x, int(wA.x), N);
    p += d;
    vWorld = p; vNormal = N; vCrest = d.y;
    gl_Position = uViewProj * vec4(p, 1.0);
}
