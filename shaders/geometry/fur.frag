#version 450
#include "geometry/geo_common.glsl"
#include "geometry/char_params.glsl"
#include "geometry/geo_gbuffer.glsl"
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vShell;
layout(location = 3) in vec4 vCurClip;
layout(location = 4) in vec4 vPrevClip;
void main(){
    vec2 cell = floor(vUV * vec2(cA.z * 2.0, cA.z));          // one hair per cell
    float n = hash12(cell);
    vec2 f = fract(vUV * vec2(cA.z * 2.0, cA.z)) - 0.5;
    float strand = 1.0 - length(f) * 2.0;                     // round strand cross-section
    if (vShell > 0.0 && (n < vShell || strand < vShell * 0.8)) discard;   // hairs thin out towards the tip
    vec3 col = cB.rgb * mix(0.35, 1.1, vShell) * (0.8 + 0.4 * n);         // roots darker (self-shadow)
    writeGBuffer(col, vNormal, 0.7, 0.0, mix(0.5, 1.0, vShell), vec3(0.0), GEO_SHADING_DEFAULT, vCurClip, vPrevClip);
}
