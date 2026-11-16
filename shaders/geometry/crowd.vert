#version 450
// Crowd: instance i walks a circle around the plaza with a bobbing "baked" walk cycle (stand-in for
// vertex animation textures). Position is evaluated at t and t - dt for correct motion vectors.
#include "geometry/geo_common.glsl"
#include "geometry/char_params.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec4 vCurClip;
layout(location = 3) out vec4 vPrevClip;
vec3 agentPos(float id, float t, out float heading){
    float r = 9.0 + mod(id * 7.31, 12.0);
    float sp = (0.6 + fract(id * 0.137) * 0.8) / r * (mod(id, 2.0) < 1.0 ? 1.0 : -1.0);   // ~1.3 m/s, both directions
    float a = id * 2.399 + t * sp;
    heading = a + (sp > 0.0 ? 1.5708 : -1.5708);
    float bob = abs(sin(t * 7.0 + id)) * 0.06;
    return vec3(cA.y + cos(a) * r, bob, cA.z + sin(a) * r);
}
void main(){
    float id = float(gl_InstanceID), h0, h1;
    vec3 b0 = agentPos(id, uTime.x, h0), b1 = agentPos(id, uTime.x - uTime.y, h1);
    mat2 R = mat2(cos(h0), sin(h0), -sin(h0), cos(h0));
    vec3 lp = aPos; lp.xz = R * lp.xz;
    vec3 p = lp + b0, pp = lp + b1;
    vec3 n = aNormal; n.xz = R * n.xz;
    vNormal = n;
    vColor = mix(vec3(0.2, 0.25, 0.5), vec3(0.6, 0.3, 0.2), fract(id * 0.618)) * (0.6 + 0.4 * fract(id * 0.31));
    gl_Position = uViewProj * vec4(p, 1.0);
    vCurClip = uUnjitteredViewProj * vec4(p, 1.0);
    vPrevClip = uPrevViewProj * vec4(pp, 1.0);
}
