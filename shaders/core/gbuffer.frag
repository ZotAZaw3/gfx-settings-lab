#version 450
#include "core/material.glsl"

layout(location = 0) out vec4 oAlbedo;     // gbuffer.albedo (sRGB)
layout(location = 1) out vec4 oNormal;     // gbuffer.normal
layout(location = 2) out vec4 oMaterial;   // gbuffer.material
layout(location = 3) out vec4 oEmissive;   // gbuffer.emissive
layout(location = 4) out vec2 oVelocity;   // scene.velocity

void main() {
    GpuMaterial m = currentMaterial();
    vec4 base = sampleBaseColor(m);
#ifdef ALPHA_TEST
    if (base.a < m.params.w) discard;
#endif
    Surface s = evaluateSurface(m, base);
    oAlbedo = vec4(s.albedo, 1.0);
    oNormal = vec4(s.normal, float(s.shadingModel) / 255.0);
    oMaterial = vec4(s.roughness, s.metallic, s.ao, 0.5);
    oEmissive = vec4(s.emissive, s.sssMask);
    oVelocity = computeVelocity();
}
