#version 450
// Upsamples an RT result (trace res) to render res and writes it into a shared lighting input
// (lighting.sunVisibility / ao / indirectDiffuse / indirectSpecular), into scene.color for the
// path tracer (remodulated by albedo), or into scene.color as a debug view.
// Modes (define): COMPOSITE_SCALAR, COMPOSITE_GI, COMPOSITE_REFL, COMPOSITE_PT, COMPOSITE_DEBUG.
#include "raytracing/rt_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 2) uniform sampler2D uMaterial;  // gbuffer.material (reflection confidence)
layout(binding = 4) uniform sampler2D uSrc;       // RT result, trace res, linear clamp
layout(binding = 5) uniform sampler2D uAlbedo;    // path tracer primary albedo (COMPOSITE_PT)

void main() {
    vec4 v = textureLod(uSrc, vUV, 0.0);
#if defined(COMPOSITE_SCALAR)
    oColor = vec4(v.r);
#elif defined(COMPOSITE_GI)
    oColor = vec4(max(v.rgb, vec3(0.0)), 1.0);
#elif defined(COMPOSITE_REFL)
    float rough = textureLod(uMaterial, vUV, 0.0).r;
    float cutoff = effectParams.y;
    // fade out over the last 10% of the roughness range so there is no hard seam to the fallback
    float conf = 1.0 - smoothstep(cutoff * 0.9, cutoff, rough);
    oColor = vec4(max(v.rgb, vec3(0.0)), conf);
#elif defined(COMPOSITE_PT)
    oColor = vec4(max(v.rgb, vec3(0.0)) * textureLod(uAlbedo, vUV, 0.0).rgb, 1.0);
#elif defined(COMPOSITE_DEBUG)
    oColor = (misc.x & 1) != 0 ? vec4(v.rrr, 1.0) : vec4(v.rgb, 1.0);
#else
    oColor = v;
#endif
}
