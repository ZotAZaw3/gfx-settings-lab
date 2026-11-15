#version 450
// shadow_depth.vert - depth-only pass for one cascade, drawn through SceneRenderer::drawScene.
// DrawUBO.uPassViewProj holds the cascade matrix (standard Z: 0 near, 1 far).
#include "common/draw.glsl"
layout(location = 0) in vec3 aPosition;
layout(location = 3) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = uPassViewProj * uWorld * vec4(aPosition, 1.0);
}
