#version 450
// One blade = triangle strip of (2 * segments + 1) vertices, bent along a quadratic curve.
#include "geometry/geo_common.glsl"
#include "geometry/grass_common.glsl"
layout(std430, binding = 8) readonly buffer Blades { Blade blades[]; };
layout(location = 0) out vec3 vNormal;
layout(location = 1) out float vT;
layout(location = 2) out float vVar;
layout(location = 3) out vec4 vCurClip;
layout(location = 4) out vec4 vPrevClip;
void main(){
    Blade b = blades[gl_InstanceID];
    int segs = int(gB.y);
    int v = gl_VertexID;
    float t = float(v / 2) / float(segs);                 // 0 root .. 1 tip
    float side = (v == 2 * segs) ? 0.0 : ((v & 1) == 0 ? -1.0 : 1.0);
    float width = b.params.y * (1.0 - t);
    vec3 dir = vec3(cos(b.posRot.w), 0.0, sin(b.posRot.w));
    vec3 across = vec3(-dir.z, 0.0, dir.x);
    float bend = b.params.z;
    vec3 root = b.posRot.xyz;
    // wind: scrolling noise pushes the tip; stiffness grows towards the root (t^2)
    vec2 windDir = normalize(vec2(1.0, 0.4));
    float gust = (gB.z > 0.5) ? (valueNoise(root.xz * 0.08 + windDir * uTime.x * 0.6) - 0.3) * 0.9 : 0.0;
    vec3 tipOffset = dir * bend * b.params.x + vec3(windDir.x, 0.0, windDir.y) * gust * b.params.x;
    // interaction: bend away from the camera/player when it walks through
    if (gB.w > 0.5){
        vec3 away = root - uCameraPos.xyz; away.y = 0.0;
        float d = length(away);
        float push = (1.0 - smoothstep(0.5, 2.5, d)) * step(abs(uCameraPos.y - root.y), 3.0);
        tipOffset += (d > 1e-3 ? away / d : vec3(0.0)) * push * b.params.x * 1.2;
    }
    vec3 p = root + vec3(0.0, b.params.x * t * (1.0 - 0.35 * t * length(tipOffset) / max(b.params.x, 1e-3)), 0.0)
           + tipOffset * t * t + across * width * side;
    vec3 tangent = normalize(vec3(0.0, b.params.x, 0.0) + tipOffset * 2.0 * t);
    vNormal = normalize(cross(across, tangent));
    vT = t; vVar = b.params.w;
    gl_Position = uViewProj * vec4(p, 1.0);
    vCurClip = uUnjitteredViewProj * vec4(p, 1.0);
    vPrevClip = uPrevViewProj * vec4(p, 1.0);
}
