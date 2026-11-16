#version 450
#include "geometry/geo_common.glsl"
#include "geometry/pom_params.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec2 aUV;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vViewTS;     // surface -> eye, tangent space
layout(location = 2) out vec3 vLightTS;    // towards sun, tangent space
layout(location = 3) out vec4 vCurClip;
layout(location = 4) out vec4 vPrevClip;
void main(){
    vec4 w = pModel * vec4(aPos, 1.0);
    vUV = aUV * pP1.z;
    vec3 V = uCameraPos.xyz - w.xyz;
    vViewTS = vec3(dot(V, pT.xyz), dot(V, pB.xyz), dot(V, pN.xyz));
    vec3 L = uSunDirection.xyz;
    vLightTS = vec3(dot(L, pT.xyz), dot(L, pB.xyz), dot(L, pN.xyz));
    gl_Position = uViewProj * w;
    vCurClip = uUnjitteredViewProj * w;
    vPrevClip = uPrevViewProj * w;
}
