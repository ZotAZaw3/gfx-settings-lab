// PLACEHOLDER owned by the lighting thread: replaced by the real PBR / shadows / GI implementation.
// Contract: vec3 shadeSurface(Surface s, vec3 V, vec2 uv) returns lit HDR radiance INCLUDING emissive.
#ifndef GFX_SHADE_GLSL
#define GFX_SHADE_GLSL
#include "common/surface.glsl"

const float PI = 3.14159265;

float D_GGX(float NoH, float a) { float a2 = a * a; float d = NoH * NoH * (a2 - 1.0) + 1.0; return a2 / (PI * d * d); }
float V_SmithGGX(float NoV, float NoL, float a) {
    float k = a * 0.5;
    return 0.25 / ((NoV * (1.0 - k) + k) * (NoL * (1.0 - k) + k));
}
vec3 F_Schlick(float VoH, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0); }

vec3 brdf(Surface s, vec3 V, vec3 L) {
    vec3 H = normalize(V + L);
    float NoL = max(dot(s.normal, L), 0.0), NoV = max(dot(s.normal, V), 1e-4), NoH = max(dot(s.normal, H), 0.0), VoH = max(dot(V, H), 0.0);
    float a = s.roughness * s.roughness;
    vec3 f0 = mix(vec3(0.04), s.albedo, s.metallic);
    vec3 F = F_Schlick(VoH, f0);
    vec3 spec = D_GGX(NoH, a) * V_SmithGGX(NoV, NoL, a) * F;
    vec3 diff = (1.0 - F) * (1.0 - s.metallic) * s.albedo / PI;
    return (diff + spec) * NoL;
}

vec3 shadeSurface(Surface s, vec3 V, vec2 uv) {
    if (s.shadingModel == SHADING_UNLIT) return s.albedo + s.emissive;
    vec3 L = normalize(uSunDirection.xyz);
    vec3 color = brdf(s, V, L) * uSunColor.rgb * uSunDirection.w;
    int count = int(uCounts.x);
    for (int i = 0; i < count; ++i) {
        GpuLight l = lights[i];
        vec3 d = l.positionRange.xyz - s.position;
        float dist = length(d);
        vec3 Ll = d / max(dist, 1e-4);
        float att = 1.0 / (1.0 + dist * dist);
        att *= clamp(1.0 - pow(dist / l.positionRange.w, 4.0), 0.0, 1.0);
        if (l.directionType.w > 0.5) {
            float cd = dot(-Ll, normalize(l.directionType.xyz));
            att *= smoothstep(l.cone.y, l.cone.x, cd);
        }
        color += brdf(s, V, Ll) * l.colorIntensity.rgb * l.colorIntensity.a * att;
    }
    // Hemisphere ambient
    vec3 sky = vec3(0.35, 0.45, 0.6), ground = vec3(0.15, 0.13, 0.1);
    vec3 ambient = mix(ground, sky, s.normal.y * 0.5 + 0.5) * 0.6;
    vec3 f0 = mix(vec3(0.04), s.albedo, s.metallic);
    color += ambient * (s.albedo * (1.0 - s.metallic) + f0 * 0.3) * s.ao;
    return color + s.emissive;
}
#endif
