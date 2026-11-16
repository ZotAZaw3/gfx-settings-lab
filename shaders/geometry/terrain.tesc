#version 450
// Adaptive tessellation control (hull) shader: edge level = projected edge length / target px.
// Shared edges get identical levels (computed from the same two corners) -> no cracks.
#include "geometry/geo_common.glsl"
#include "geometry/terrain_params.glsl"
layout(vertices = 4) out;
layout(location = 0) in vec3 vPos[];
layout(location = 0) out vec3 tcPos[];

float edgeLevel(vec3 a, vec3 b){
    a.y = terrainHeight(terrainUV(a)); b.y = terrainHeight(terrainUV(b));
    vec3 m = 0.5 * (a + b); float r = 0.5 * distance(a, b);
    vec4 c = uUnjitteredViewProj * vec4(m, 1.0);
    float px = 2.0 * r / max(c.w, 1e-3) * tB.x;   // projected edge length (px)
    return clamp(px / max(tA.z, 1.0), 1.0, tA.w);
}
bool patchOutside(){
    vec3 lo = min(min(vPos[0], vPos[1]), min(vPos[2], vPos[3]));
    vec3 hi = max(max(vPos[0], vPos[1]), max(vPos[2], vPos[3]));
    lo.y = -0.15 * tA.y; hi.y = tA.y;              // full displacement range (conservative)
    int oL = 0, oR = 0, oB = 0, oT = 0, oN = 0;
    for (int i = 0; i < 8; ++i){
        vec3 p = vec3((i & 1) != 0 ? hi.x : lo.x, (i & 2) != 0 ? hi.y : lo.y, (i & 4) != 0 ? hi.z : lo.z);
        vec4 c = uUnjitteredViewProj * vec4(p, 1.0);
        oL += int(c.x < -c.w); oR += int(c.x > c.w); oB += int(c.y < -c.w); oT += int(c.y > c.w);
        oN += int(c.z > c.w);                         // reversed-Z: closer than the near plane
    }
    return oL == 8 || oR == 8 || oB == 8 || oT == 8 || oN == 8;
}
void main(){
    tcPos[gl_InvocationID] = vPos[gl_InvocationID];
    if (gl_InvocationID == 0){
        if (tB.y > 0.5 && patchOutside()){
            gl_TessLevelOuter[0] = 0.0; gl_TessLevelOuter[1] = 0.0; gl_TessLevelOuter[2] = 0.0; gl_TessLevelOuter[3] = 0.0;
            gl_TessLevelInner[0] = 0.0; gl_TessLevelInner[1] = 0.0;
        } else {
            // quad domain: outer0 = u=0 edge (v0-v3), outer1 = v=0 edge (v0-v1), outer2 = u=1 (v1-v2), outer3 = v=1 (v3-v2)
            gl_TessLevelOuter[0] = edgeLevel(vPos[0], vPos[3]);
            gl_TessLevelOuter[1] = edgeLevel(vPos[0], vPos[1]);
            gl_TessLevelOuter[2] = edgeLevel(vPos[1], vPos[2]);
            gl_TessLevelOuter[3] = edgeLevel(vPos[3], vPos[2]);
            gl_TessLevelInner[0] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
            gl_TessLevelInner[1] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);
        }
    }
}
