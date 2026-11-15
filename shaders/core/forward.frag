#version 450
// Forward shading (MSAA path and transparent meshes). Lighting comes from lighting/shade.glsl.
#include "core/material.glsl"
#include "lighting/shade.glsl"

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec2 oVelocity;

void main() {
    GpuMaterial m = currentMaterial();
    vec4 base = sampleBaseColor(m);
#ifdef ALPHA_TEST
    if (base.a < m.params.w) discard;
#endif
    Surface s = evaluateSurface(m, base);
    vec3 V = normalize(uCameraPos.xyz - s.position);
    vec2 uv = gl_FragCoord.xy * uRenderSize.zw;
    vec3 color = shadeSurface(s, V, uv);       // includes emissive
    oColor = vec4(color, base.a);
    oVelocity = computeVelocity();
}
