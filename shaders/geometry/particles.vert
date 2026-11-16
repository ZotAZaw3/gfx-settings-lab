#version 450
// Expands particle (base + gl_VertexID / 6) into a camera-facing quad. No vertex buffer.
#include "geometry/geo_common.glsl"
#include "geometry/particle_common.glsl"
layout(std430, binding = 8) readonly buffer Particles { Particle p[]; };
layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec3 vColor;
layout(location = 2) out float vT;
void main(){
    Particle q = p[uint(gl_VertexID / 6) + uint(eB.y)];
    const vec2 c[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));
    vec2 k = c[gl_VertexID % 6];
    float t = clamp(q.velAge.w / max(q.posLife.w, 1e-3), 0.0, 1.0);
    int type = int(eA.w);
    float size = q.color.w * ((type == 1) ? (0.5 + 2.5 * t) : (type == 2 ? (1.0 - 0.6 * t) : 1.0));
    vec3 right = uInvView[0].xyz, up = uInvView[1].xyz;
    vec3 w = q.posLife.xyz + (right * k.x + up * k.y) * size;
    vCorner = k; vColor = q.color.rgb; vT = t;
    gl_Position = uViewProj * vec4(w, 1.0);
}
