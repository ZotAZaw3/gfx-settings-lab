// ao_common.glsl - inputs shared by the SSAO / HBAO / GTAO kernels (docs/ambient_occlusion.md)
#ifndef AO_COMMON_GLSL
#define AO_COMMON_GLSL
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 1) uniform sampler2D uGNormal;
layout(binding = 3) uniform sampler2D uDepth;
layout(r8, binding = 0) uniform writeonly image2D uAOOut;
layout(std140, binding = 3) uniform AOPass { vec4 uAOPass; };   // xy = AO target size (may be half res)
#define uOutSize uAOPass.xy

vec3 aoViewPos(vec2 uv) { return viewPosFromDepth(uv, max(textureLod(uDepth, uv, 0.0).r, 1e-7)); }
vec3 aoViewNormal(vec2 uv) { return normalize(mat3(uView) * textureLod(uGNormal, uv, 0.0).xyz); }
// pixels-per-meter at view depth z (for converting a world radius to screen)
float projScale() { return uProj[1][1] * 0.5 * uRenderSize.y; }
#endif
