#version 450
// Terrain material: normal from the heightmap (per pixel), height-blended layers, optional triplanar cliffs.
#include "geometry/geo_common.glsl"
#include "geometry/terrain_params.glsl"
#include "geometry/geo_gbuffer.glsl"
layout(location = 0) in vec3 vPos;
layout(location = 1) in vec4 vCurClip;
layout(location = 2) in vec4 vPrevClip;

vec3 layerColor(int i, vec2 p){
    float n = fbm(p * 0.7);
    if (i == 0) return mix(vec3(0.16, 0.26, 0.07), vec3(0.24, 0.34, 0.10), n);    // grass
    if (i == 1) return mix(vec3(0.30, 0.28, 0.26), vec3(0.46, 0.44, 0.41), n);    // rock
    if (i == 2) return mix(vec3(0.55, 0.49, 0.36), vec3(0.66, 0.60, 0.46), n);    // sand / shore
    return mix(vec3(0.80), vec3(0.95), n);                                         // snow
}
void main(){
    vec2 uv = terrainUV(vPos);
    vec2 texel = 1.0 / vec2(textureSize(tHeightMap, 0));
    float hL = terrainHeight(uv - vec2(texel.x, 0)), hR = terrainHeight(uv + vec2(texel.x, 0));
    float hD = terrainHeight(uv - vec2(0, texel.y)), hU = terrainHeight(uv + vec2(0, texel.y));
    float stepW = tA.x * texel.x;
    vec3 N = normalize(vec3(hL - hR, 2.0 * stepW, hD - hU));

    float slope = 1.0 - N.y, h = vPos.y / max(tA.y, 1e-3);
    float w[4] = float[](1.0, smoothstep(0.12, 0.3, slope), 1.0 - smoothstep(0.0, 0.04, h), smoothstep(0.6, 0.75, h) * (1.0 - smoothstep(0.3, 0.5, slope)));
    int layers = int(tB.z);
    vec2 p = vPos.xz;
    vec3 col = layerColor(0, p);
    float best = w[0] * 0.5 + fbm(p * 1.3) * 0.3;
    for (int i = 1; i < 4; ++i){
        if (i >= layers) break;
        vec2 lp = p;
        if (i == 1 && tB.w > 0.5) lp = (abs(N.x) > abs(N.z)) ? vPos.zy : vPos.xy;      // triplanar-ish: project cliffs sideways
        float hi = w[i] + fbm(lp * 1.3 + float(i) * 5.0) * 0.3;                       // height-based blend
        col = mix(col, layerColor(i, lp), smoothstep(best - 0.08, best + 0.08, hi));
        best = max(best, hi);
    }
    if (tC.z > 0.5){
        vec3 fn = normalize(cross(dFdx(vPos), dFdy(vPos)));                           // facet normal -> see triangles
        col *= 0.45 + 0.55 * abs(dot(fn, normalize(vec3(0.4, 1.0, 0.3))));
    }
    writeGBuffer(col, N, 0.9, 0.0, 1.0, vec3(0.0), GEO_SHADING_DEFAULT, vCurClip, vPrevClip);
}
