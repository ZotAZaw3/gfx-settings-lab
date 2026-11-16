#version 450
// forward_lite.vert - simple forward scene pass used for planar reflections and probe capture
// (DrawMode::Custom). Core binds the vertex buffer, DrawUBO and material textures 0-5.
#include "common/draw.glsl"
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec2 aUV;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
void main() {
    vec4 wp = uWorld * vec4(aPosition, 1.0);
    vWorldPos = wp.xyz;
    vNormal = mat3(uWorld) * aNormal;
    vUV = aUV;
    gl_Position = uPassViewProj * wp;
}
