// pbr_common.glsl - Cook-Torrance GGX BRDF, metal/roughness workflow. See docs/pbr_shading.md
#ifndef PBR_COMMON_GLSL
#define PBR_COMMON_GLSL

// Uses Surface from common/surface.glsl.

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float d = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * d * d + 1e-7);
}
// Height-correlated Smith visibility = G / (4 NoL NoV)
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (gv + gl + 1e-7);
}
vec3 F_Schlick(vec3 f0, float VoH) {
    float f = pow(1.0 - VoH, 5.0);
    return f0 + (1.0 - f0) * f;
}
vec3 F_SchlickRoughness(vec3 f0, float NoV, float rough) {
    return f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - NoV, 5.0);
}
// Low quality: energy-normalized Blinn-Phong
float D_BlinnPhong(float NoH, float a) {
    float n = 2.0 / max(a * a, 1e-4) - 2.0;
    return (n + 2.0) / (2.0 * PI) * pow(NoH, n);
}

// Lagarde: specular occlusion from AO
float specularOcclusion(float NoV, float ao, float roughness) {
    return clamp(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}
// Jimenez GTAO multi-bounce fit: lighter albedo => AO less dark
vec3 aoMultiBounce(float ao, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(ao), ((ao * a + b) * ao + c) * ao);
}

// Direct light: returns (diffuse, specular) separately so SSS can blur diffuse only.
void evaluateBRDF(Surface s, vec3 V, vec3 L, vec3 radiance, int brdf,
                  inout vec3 diffuse, inout vec3 specular, float wrap) {
    vec3 N = s.normal;
    vec3 H = normalize(V + L);
    float NoLraw = dot(N, L);
    float NoL = clamp(NoLraw, 0.0, 1.0);
    float NoV = clamp(abs(dot(N, V)), 1e-4, 1.0);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float VoH = clamp(dot(V, H), 0.0, 1.0);
    float a = max(s.roughness * s.roughness, 0.002);

    vec3 f0 = mix(vec3(0.04), s.albedo, s.metallic);
    vec3 F = F_Schlick(f0, VoH);
    vec3 spec;
    if (brdf == 0) spec = F * D_BlinnPhong(NoH, a) * 0.25;
    else           spec = F * D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a);

    vec3 kd = (1.0 - F) * (1.0 - s.metallic);
    // wrap lighting softens the terminator for SSS materials at low quality
    float NoLd = wrap > 0.0 ? clamp((NoLraw + wrap) / ((1.0 + wrap) * (1.0 + wrap)), 0.0, 1.0) : NoL;
    diffuse  += kd * s.albedo / PI * radiance * NoLd;
    specular += spec * radiance * NoL;
}

// Clear coat: a second, smooth dielectric GGX layer on top (car paint, lacquer). Returns the
// coat specular and attenuates the base by (1 - Fc).
vec3 clearCoatLobe(vec3 N, vec3 V, vec3 L, vec3 radiance, float coatRough, out float Fc) {
    vec3 H = normalize(V + L);
    float NoL = clamp(dot(N, L), 0.0, 1.0), NoH = clamp(dot(N, H), 0.0, 1.0), VoH = clamp(dot(V, H), 0.0, 1.0);
    float a = coatRough * coatRough;
    Fc = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
    float Vk = 0.25 / max(VoH * VoH, 1e-4);   // Kelemen visibility
    return vec3(D_GGX(NoH, a) * Vk * Fc) * radiance * NoL;
}

float lightFalloff(float dist, float range) {
    float r = dist / range;
    float w = clamp(1.0 - r * r * r * r, 0.0, 1.0);
    return w * w / (dist * dist + 1.0);
}

#endif
