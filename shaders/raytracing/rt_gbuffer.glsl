// G-buffer adapter: the only place that knows the core G-buffer layout.
// All RT passes bind: 0 scene.depth, 1 gbuffer.normal, 2 gbuffer.material, 3 gbuffer.albedo.
#ifndef RT_GBUFFER_GLSL
#define RT_GBUFFER_GLSL

layout(binding = 0) uniform sampler2D gbDepthTex;    // reversed-Z D32F
layout(binding = 1) uniform sampler2D gbNormalTex;   // RGBA16F, xyz world normal
layout(binding = 2) uniform sampler2D gbMaterialTex; // RGBA8: r roughness, g metallic, b AO, a reflectance
layout(binding = 3) uniform sampler2D gbAlbedoTex;   // RGBA8_SRGB: rgb base color (linear when sampled)

struct GSample { bool valid; float depth; vec3 pos; vec3 n; vec3 albedo; float roughness; float metallic; };

// Full-res texel for a trace-res pixel (trace res may be scaled down).
ivec2 fullTexel(ivec2 tracePx) {
    return clamp(ivec2((vec2(tracePx) + 0.5) * vec2(sizes.zw) / vec2(sizes.xy)), ivec2(0), sizes.zw - 1);
}
vec2 fullUV(ivec2 fullPx) { return (vec2(fullPx) + 0.5) / vec2(sizes.zw); }

vec3 worldFromDepth(vec2 uv, float depth, mat4 invVP) {
    vec4 p = invVP * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return p.xyz / p.w;
}

GSample loadG(ivec2 fullPx) {
    GSample g;
    g.depth = texelFetch(gbDepthTex, fullPx, 0).r;
    g.valid = g.depth > 0.0; // reversed-Z: 0 = far/sky
    g.pos = worldFromDepth(fullUV(fullPx), g.depth, invViewProj);
    g.n = normalize(texelFetch(gbNormalTex, fullPx, 0).xyz);
    vec4 m = texelFetch(gbMaterialTex, fullPx, 0);
    g.roughness = clamp(m.r, 0.02, 1.0);
    g.metallic = m.g;
    g.albedo = texelFetch(gbAlbedoTex, fullPx, 0).rgb;
    return g;
}

// Camera ray through a full-res UV (for the path tracer's primary rays).
void cameraRay(vec2 uv, out vec3 o, out vec3 d) {
    vec3 pNear = worldFromDepth(uv, 1.0, invViewProjCam);
    vec3 pMid  = worldFromDepth(uv, 0.5, invViewProjCam);
    o = camPos.xyz;
    d = normalize(pMid - pNear);
}

#endif
