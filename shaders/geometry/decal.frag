#version 450
// Deferred box decal: reconstruct world position from scene depth, project into decal space,
// modify the G-buffer before lighting so decals are lit/shadowed like the surface.
#include "geometry/geo_common.glsl"
#include "geometry/decal_common.glsl"
layout(location = 0) out vec4 gAlbedo;     // alpha-blended (rgb only)
layout(location = 1) out vec4 gNormal;     // opaque, rgb only (w = shading model untouched)
layout(location = 2) out vec4 gMaterial;   // opaque, r (roughness) only on High
layout(binding = 0) uniform sampler2D tSceneDepth;   // not attached in this pass -> safe to sample
layout(binding = 1) uniform sampler2D tNormalCopy;
layout(binding = 2) uniform sampler2D tMaterialCopy;
layout(location = 0) flat in int vIndex;
layout(location = 1) flat in vec3 vFaceN;
layout(location = 2) in vec3 vWorld;

// procedural decal "atlas": 0 scorch, 1 bullet hole, 2 paint splat, 3 puddle
vec4 decalTex(int kind, vec2 uv, out float rough, out vec2 slope){
    vec2 c = uv * 2.0 - 1.0; float r = length(c); rough = 0.8; slope = vec2(0.0);
    if (kind == 0){ float n = fbm(uv * 6.0); return vec4(vec3(0.02), smoothstep(1.0, 0.2, r + n * 0.4) * 0.9); }
    if (kind == 1){ float a = smoothstep(0.35, 0.3, r); float ring = smoothstep(0.5, 0.35, r) - a;
                    slope = c * ring * 3.0; rough = 0.6; return vec4(mix(vec3(0.25), vec3(0.01), a), max(a, ring * 0.7)); }
    if (kind == 2){ float n = fbm(uv * 4.0 + 3.0); return vec4(0.7, 0.04, 0.06, smoothstep(0.75, 0.6, r + (n - 0.5) * 0.6)); }
    float n = fbm(uv * 3.0); rough = 0.03;
    return vec4(vec3(0.04, 0.05, 0.06), smoothstep(0.9, 0.7, r + n * 0.3) * 0.75);
}
void main(){
    // keep back faces only (works with the camera inside the box, shades each pixel once)
    if (dot(vFaceN, vWorld - uCameraPos.xyz) < 0.0) discard;
    vec2 uv = gl_FragCoord.xy * uRenderSize.zw;
    float d = texelFetch(tSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (d <= 1e-7) discard;
    vec3 wp = reconstructWorldPos(uv, d);
    Decal dc = decals[vIndex];
    vec3 lp = (dc.invModel * vec4(wp, 1.0)).xyz;
    if (any(greaterThan(abs(lp), vec3(0.5)))) discard;
    float rough; vec2 slope;
    vec4 c = decalTex(int(dc.params.x), lp.xz + 0.5, rough, slope);
    float life = 1.0 - smoothstep(dc.params.z * 0.8, dc.params.z, dc.params.y);
    float fade = c.a * life * dc.params.w * (1.0 - smoothstep(0.3, 0.5, abs(lp.y)));
    vec4 nr = texelFetch(tNormalCopy, ivec2(gl_FragCoord.xy), 0);
    vec3 N = normalize(nr.xyz);
    vec3 projDir = normalize(dc.model[1].xyz);
    if (dA.x > 0.5) fade *= smoothstep(0.2, 0.5, abs(dot(N, projDir)));   // angle fade: no stretching on walls
    if (fade <= 0.002) discard;
    gAlbedo = vec4(c.rgb, fade);
    vec3 T = normalize(dc.model[0].xyz), B = normalize(dc.model[2].xyz);
    vec3 nn = normalize(N + (T * slope.x + B * slope.y));
    gNormal = vec4(dA.x > 0.5 ? normalize(mix(N, nn, fade)) : N, nr.w);
    float r0 = texelFetch(tMaterialCopy, ivec2(gl_FragCoord.xy), 0).r;
    gMaterial = vec4(mix(r0, rough, fade), 0.0, 0.0, 0.0);
}
