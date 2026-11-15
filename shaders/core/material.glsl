// Shared material evaluation for core scene fragment shaders.
#ifndef GFX_MATERIAL_GLSL
#define GFX_MATERIAL_GLSL
#include "common/draw.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vCurClip;
layout(location = 5) in vec4 vPrevClip;

GpuMaterial currentMaterial() { return materials[uIds.x]; }

vec4 sampleBaseColor(GpuMaterial m) {
    vec4 c = m.baseColor;
    if (m.tex0.x >= 0) c *= texture(tBaseColor, vUV, uDrawParams.x);
    return c;
}

Surface evaluateSurface(GpuMaterial m, vec4 baseColor) {
    Surface s;
    s.position = vWorldPos;
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    if (m.tex0.y >= 0) {
        vec3 T = normalize(vTangent.xyz - N * dot(N, vTangent.xyz));
        vec3 B = cross(N, T) * vTangent.w;
        vec3 tn = texture(tNormalMap, vUV, uDrawParams.x).xyz * 2.0 - 1.0;
        tn.xy *= m.params.y;
        N = normalize(mat3(T, B, N) * tn);
    }
    s.normal = N;
    s.albedo = baseColor.rgb;
    s.roughness = m.emissiveRough.a;
    s.metallic = m.params.x;
    if (m.tex0.z >= 0) {
        vec4 mr = texture(tMetalRough, vUV, uDrawParams.x);
        s.roughness *= mr.g;
        s.metallic *= mr.b;
    }
    s.roughness = clamp(s.roughness, 0.02, 1.0);
    s.ao = 1.0;
    if (m.tex0.w >= 0) s.ao = mix(1.0, texture(tOcclusion, vUV).r, m.params.z);
    s.emissive = m.emissiveRough.rgb;
    if (m.tex1.x >= 0) s.emissive *= texture(tEmissive, vUV).rgb;
    s.shadingModel = m.tex1.w;
    s.sssMask = m.tex1.w == SHADING_SUBSURFACE ? clamp(m.sss.a / 5.0, 0.0, 1.0) : 0.0;
    return s;
}

vec2 computeVelocity() {
    vec2 cur = vCurClip.xy / vCurClip.w;
    vec2 prev = vPrevClip.xy / vPrevClip.w;
    return cur - prev;
}
#endif
