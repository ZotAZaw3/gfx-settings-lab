// geometry/geo_gbuffer.glsl - the ONLY place in the geometry shaders that knows the core G-buffer layout
// (MODULE_API section 5). Attachment order must match geo::beginGBufferPass() in geo_util.cpp.
#ifndef GEO_GBUFFER_GLSL
#define GEO_GBUFFER_GLSL
layout(location = 0) out vec4 gAlbedo;     // gbuffer.albedo   RGBA8_SRGB: rgb base color, a specular occlusion
layout(location = 1) out vec4 gNormal;     // gbuffer.normal   RGBA16F: xyz world normal, w shading model / 255
layout(location = 2) out vec4 gMaterial;   // gbuffer.material RGBA8: r roughness, g metallic, b AO, a reflectance
layout(location = 3) out vec4 gEmissive;   // gbuffer.emissive RGBA16F: rgb emissive, a SSS mask
layout(location = 4) out vec2 gVelocity;   // scene.velocity   RG16F: NDC(cur) - NDC(prev), unjittered

#define GEO_SHADING_DEFAULT 0
#define GEO_SHADING_FOLIAGE 2

void writeGBuffer(vec3 albedo, vec3 N, float roughness, float metallic, float ao, vec3 emissive,
                  int shadingModel, vec4 curClip, vec4 prevClip){
    gAlbedo   = vec4(albedo, 1.0);
    gNormal   = vec4(normalize(N), float(shadingModel) / 255.0);
    gMaterial = vec4(roughness, metallic, ao, 0.5);
    gEmissive = vec4(emissive, 0.0);
    gVelocity = curClip.xy / curClip.w - prevClip.xy / prevClip.w;
}
#endif
