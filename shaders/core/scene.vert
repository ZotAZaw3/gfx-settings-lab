#version 450
// Core scene vertex shader: G-buffer, forward and depth passes (see common/draw.glsl).
#include "common/draw.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vCurClip;    // unjittered, for velocity
layout(location = 5) out vec4 vPrevClip;

void main() {
    vec4 world = uWorld * vec4(aPosition, 1.0);
    mat3 nrm = transpose(inverse(mat3(uWorld)));
    vWorldPos = world.xyz;
    vNormal = normalize(nrm * aNormal);
    vTangent = vec4(normalize(mat3(uWorld) * aTangent.xyz), aTangent.w);
    vUV = aUV;
    vCurClip = uUnjitteredViewProj * world;
    vPrevClip = uPassPrevViewProj * (uPrevWorld * vec4(aPosition, 1.0));
    gl_Position = uPassViewProj * world;
}
