#version 450
// Fullscreen triangle, no vertex buffer. Draw 3 vertices.
layout(location = 0) out vec2 vUV;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = uv;                                   // (0,0) top-left
    gl_Position = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
