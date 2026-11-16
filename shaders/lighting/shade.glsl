// shade.glsl - THE shared surface shading entry point (owned by the lighting thread).
//
//   vec3 shadeSurface(Surface s, vec3 V, vec2 uv)
//
// Called by the deferred resolve (lighting/deferred_lighting.comp) and by core's forward/MSAA path.
// Result = direct + indirect + s.emissive (callers must not add emissive again).
// Resources (bound by lighting.pbr in Module::bindShadingResources):
//   tex 8  shadow cascades (compare)      tex 12 lighting.contactShadows (R8, 1 = lit)
//   tex 9  shadow cascades (raw, PCSS)    tex 13 lighting.indirectSpecular (rgb radiance, a confidence)
//   tex 10 lighting.sunVisibility (RT)    tex 14 lighting.indirectDiffuse (rgb irradiance/PI, a confidence)
//   tex 11 lighting.ao (indirect only)    tex 15 prefiltered environment cube (GGX mips)
//   UBO 6  LightingUBO                    SSBO 4 SH probe grid   SSBO 5 split-sum BRDF LUT
// Absent targets are bound as 1x1 fallbacks (white for visibility/AO, black a=0 for indirect).
// Math: docs/pbr_shading.md, docs/light_quality.md, docs/global_illumination.md, docs/reflections.md
#ifndef SHADE_GLSL
#define SHADE_GLSL

#include "common/surface.glsl"
#include "lighting/lighting_common.glsl"
#include "lighting/pbr_common.glsl"
#include "lighting/shadow_sample.glsl"

layout(binding = 10) uniform sampler2D   tSunVisibility;
layout(binding = 11) uniform sampler2D   tAO;
layout(binding = 12) uniform sampler2D   tContactShadows;
layout(binding = 13) uniform sampler2D   tIndirectSpecular;
layout(binding = 14) uniform sampler2D   tIndirectDiffuse;
layout(binding = 15) uniform samplerCube tEnvPrefiltered;

// L2 SH probe grid: 7 vec4 per probe (27 coefficients), probe index = x + y*dx + z*dx*dy
layout(std430, binding = 4) readonly buffer ProbeSSBO { vec4 probeSH[]; };
// Split-sum DFG LUT, BRDF_LUT_SIZE^2 entries of (A, B): x = NoV, y = roughness
#define BRDF_LUT_SIZE 32
layout(std430, binding = 5) readonly buffer BrdfLutSSBO { vec2 brdfLut[]; };

// The tiled deferred resolve overrides these to iterate its per-tile light list.
#ifndef LIGHT_LIST_COUNT
#define LIGHT_LIST_COUNT() min(int(uCounts.x), uLightCfg.y)
#define LIGHT_LIST_GET(i)  (i)
#endif

vec2 sampleBrdfLut(float NoV, float rough) {
    vec2 p = clamp(vec2(NoV, rough) * float(BRDF_LUT_SIZE) - 0.5, vec2(0.0), vec2(float(BRDF_LUT_SIZE - 1)));
    ivec2 i0 = ivec2(floor(p));
    ivec2 i1 = min(i0 + 1, ivec2(BRDF_LUT_SIZE - 1));
    vec2 f = p - vec2(i0);
    vec2 a = mix(brdfLut[i0.y * BRDF_LUT_SIZE + i0.x], brdfLut[i0.y * BRDF_LUT_SIZE + i1.x], f.x);
    vec2 b = mix(brdfLut[i1.y * BRDF_LUT_SIZE + i0.x], brdfLut[i1.y * BRDF_LUT_SIZE + i1.x], f.x);
    return mix(a, b, f.y);
}

// ---- Spherical harmonics irradiance (docs/global_illumination.md) ----
// Returns irradiance / PI so that diffuse = albedo * result.
vec3 shIrradiance(vec3 c[9], vec3 n, bool l2) {
    const float c0 = 0.886227, c1 = 1.023328, c2 = 0.858086, c3 = 0.247708, c4 = 0.429043;
    vec3 e = c0 * c[0] + c1 * (c[1] * n.y + c[2] * n.z + c[3] * n.x);
    if (l2) {
        e += c2 * (c[4] * n.x * n.y + c[5] * n.y * n.z + c[7] * n.x * n.z)
           + c3 * c[6] * (3.0 * n.z * n.z - 1.0)
           + c4 * c[8] * (n.x * n.x - n.y * n.y);
    }
    return max(e, vec3(0.0)) / PI;
}

void loadProbe(ivec3 p, float w, inout vec4 acc[7]) {
    int idx = (p.x + p.y * uProbeGridDim.x + p.z * uProbeGridDim.x * uProbeGridDim.y) * 7;
    for (int k = 0; k < 7; ++k) acc[k] += probeSH[idx + k] * w;
}

vec3 probeIrradiance(vec3 worldPos, vec3 n, bool l2) {
    ivec3 dim = uProbeGridDim.xyz;
    // push the lookup along the normal to reduce leaking through thin walls
    vec3 g = (worldPos + n * uGIParams.z - uProbeGridMin.xyz) / uProbeGridSize.xyz * vec3(dim - 1);
    g = clamp(g, vec3(0.0), vec3(dim - 1));
    ivec3 i0 = ivec3(floor(g));
    ivec3 i1 = min(i0 + 1, dim - 1);
    vec3 f = g - vec3(i0);
    vec4 t[7];
    for (int k = 0; k < 7; ++k) t[k] = vec4(0.0);
    for (int c = 0; c < 8; ++c) {
        ivec3 sel = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        ivec3 p = mix(i0, i1, bvec3(sel));
        vec3 wv = mix(1.0 - f, f, vec3(sel));
        float w = wv.x * wv.y * wv.z;
        if (w > 0.0) loadProbe(p, w, t);
    }
    vec3 c[9];
    c[0] = t[0].rgb;                    c[1] = vec3(t[0].a, t[1].rg);   c[2] = vec3(t[1].ba, t[2].r);
    c[3] = t[2].gba;                    c[4] = t[3].rgb;                c[5] = vec3(t[3].a, t[4].rg);
    c[6] = vec3(t[4].ba, t[5].r);       c[7] = t[5].gba;                c[8] = t[6].rgb;
    return shIrradiance(c, n, l2);
}

vec3 diffuseIndirect(Surface s, vec2 uv) {
    int gi = uGICfg.x;
    if (gi >= 2 && uProbeGridDim.w == 0) gi = 1;            // grid not baked yet
    vec3 irr;
    if (gi == 0)      irr = uAmbient.rgb;
    else if (gi == 1) irr = mix(uGround.rgb, uAmbient.rgb, s.normal.y * 0.5 + 0.5) * 0.25
                          + textureLod(tEnvPrefiltered, s.normal, uReflParams.z).rgb * uAmbient.w;
    else              irr = probeIrradiance(s.position, s.normal, gi == 3);
    vec4 ssgi = textureLod(tIndirectDiffuse, uv, 0.0);       // SSGI or RTGI, a = confidence
    return mix(irr, ssgi.rgb, clamp(ssgi.a, 0.0, 1.0));
}

vec3 envSpecular(Surface s, vec3 V, vec2 uv) {
    vec3 R = reflect(-V, s.normal);
    if (uReflBoxMin.w > 0.0) {        // parallax-corrected (box-projected) local probe
        vec3 t1 = (uReflBoxMax.xyz - s.position) / R;
        vec3 t2 = (uReflBoxMin.xyz - s.position) / R;
        vec3 tf = max(t1, t2);
        float t = min(min(tf.x, tf.y), tf.z);
        vec3 hit = s.position + R * t;
        // rough reflections are blurry anyway: blend back to the raw direction
        R = normalize(mix(hit - uReflProbePos.xyz, R, s.roughness * s.roughness));
    }
    vec3 env = uReflCfg.x != 0
        ? textureLod(tEnvPrefiltered, R, s.roughness * uReflParams.z).rgb * uAmbient.w
        : uAmbient.rgb;
    vec4 ssr = textureLod(tIndirectSpecular, uv, 0.0);      // SSR or RT reflections
    return mix(env, ssr.rgb, clamp(ssr.a, 0.0, 1.0));
}

float sunVisibility(Surface s, vec2 uv, float viewDepth) {
    vec2 pixel = uv * uRenderSize.xy;
    float vis = uLightCfg2.z != 0 ? textureLod(tSunVisibility, uv, 0.0).r
                                  : sunShadow(s.position, s.normal, viewDepth, pixel);
    vis *= textureLod(tContactShadows, uv, 0.0).r;
    if (uShadowCfg.w > 0) vis *= capsuleShadow(s.position, sunDir(), cos(max(uSunColor.w * 4.0, 0.02)));
    return vis;
}

// Full shading, diffuse and specular returned separately (SSS blurs diffuse only).
// Emissive is NOT included here; shadeSurface() adds it.
void shadeSurfaceSplit(Surface s, vec3 V, vec2 uv, out vec3 outDiffuse, out vec3 outSpecular) {
    vec3 diffuse = vec3(0.0), specular = vec3(0.0);
    int brdf = uLightCfg.x;
    bool sss = s.shadingModel == SHADING_SUBSURFACE || s.shadingModel == SHADING_FOLIAGE;
    float wrap = (sss && uSSSCfg.x == 1) ? uSSSParams.z : 0.0;
    float viewDepth = -(uView * vec4(s.position, 1.0)).z;
    bool coat = s.shadingModel == SHADING_CLEARCOAT && uLightCfg2.w != 0;
    float coatAtten = 1.0;
    vec3 coatSpec = vec3(0.0);

    // ---- sun ----
    if (uSunDirection.w > 0.0) {
        vec3 L = sunDir();
        float vis = sunVisibility(s, uv, viewDepth);
        vec3 rad = sunRadiance();
        evaluateBRDF(s, V, L, rad * vis, brdf, diffuse, specular, wrap);
        if (coat) { float Fc; coatSpec += clearCoatLobe(s.normal, V, L, rad * vis, 0.08, Fc); coatAtten = 1.0 - Fc; }

        // thin-object transmission (ears, leaves): light through the back side
        if (sss && uSSSCfg.z != 0) {
            float back = clamp(0.3 - dot(s.normal, L), 0.0, 1.0);
            // without a thickness map, use the sss mask as inverse thickness
            float thick = (1.0 - s.sssMask) * 4.0;
            vec3 prof = exp(-thick * vec3(1.0, 2.5, 5.0));
            float shadowed = uLightCfg2.z != 0 ? 1.0 : sunShadow(s.position, -s.normal, viewDepth, uv * uRenderSize.xy);
            diffuse += s.albedo * prof * rad * back * shadowed * uSSSParams.y * 0.5;
        }
    }

    // ---- punctual lights ----
    int n = LIGHT_LIST_COUNT();
    for (int k = 0; k < n; ++k) {
        GpuLight l = lights[LIGHT_LIST_GET(k)];
        vec3 toL = l.positionRange.xyz - s.position;
        float d = length(toL);
        if (d > l.positionRange.w) continue;
        vec3 L = toL / max(d, 1e-4);
        float att = lightFalloff(d, l.positionRange.w);
        if (l.directionType.w > 0.5)
            att *= smoothstep(l.cone.y, l.cone.x, dot(-L, normalize(l.directionType.xyz)));
        if (att <= 0.0) continue;
        vec3 rad = l.colorIntensity.rgb * l.colorIntensity.w * att;
        Surface sl = s;
        if (uLightCfg.w != 0) {       // representative-point area approximation: 5 cm source
            float a = s.roughness * s.roughness;
            sl.roughness = sqrt(clamp(a + 0.05 / (2.0 * d), 0.0, 1.0));
        }
        evaluateBRDF(sl, V, L, rad, brdf, diffuse, specular, wrap);
        if (coat) { float Fc; coatSpec += clearCoatLobe(s.normal, V, L, rad, 0.08, Fc); }
    }

    // ---- indirect ----
    float NoV = clamp(dot(s.normal, V), 1e-4, 1.0);
    vec3 f0 = mix(vec3(0.04), s.albedo, s.metallic);
    float ao = s.ao;
    // SSAO/GTAO/RTAO (white when absent). Intensity is an exponent: physically based AO is subtle,
    // games usually push it (1.5-2) for readability.
    ao = min(ao, pow(clamp(textureLod(tAO, uv, 0.0).r, 0.0, 1.0), uAOParams.y));
    vec3 aoD = uAOCfg.w != 0 ? aoMultiBounce(ao, s.albedo) : vec3(ao);
    float specOcc = uAOParams.w > 0.0 ? specularOcclusion(NoV, ao, s.roughness) : 1.0;

    vec2 ab = sampleBrdfLut(NoV, s.roughness);
    vec3 FssEss = f0 * ab.x + ab.y;
    vec3 energy = vec3(1.0);
    if (uLightCfg.z != 0) energy = 1.0 + f0 * (1.0 / max(ab.x + ab.y, 1e-3) - 1.0);   // multi-scatter
    vec3 kd = (1.0 - F_SchlickRoughness(f0, NoV, s.roughness)) * (1.0 - s.metallic);
    diffuse  += kd * s.albedo * diffuseIndirect(s, uv) * aoD;
    specular += envSpecular(s, V, uv) * FssEss * specOcc;
    specular *= energy;

    if (coat) {   // coat reflects the environment too, and dims the base layer
        Surface sc = s; sc.roughness = 0.08;
        float Fv = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
        coatSpec += envSpecular(sc, V, uv) * Fv * specOcc;
        diffuse *= coatAtten; specular *= coatAtten;
        specular += coatSpec;
    }
    outDiffuse = diffuse;
    outSpecular = specular;
}

vec3 shadeSurface(Surface s, vec3 V, vec2 uv) {
    vec3 emissive = s.emissive * uGround.w;
    if (s.shadingModel == SHADING_UNLIT) return s.albedo + emissive;
    vec3 d, sp;
    shadeSurfaceSplit(s, V, uv, d, sp);
    return d + sp + emissive;
}

#endif
