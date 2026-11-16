#version 450
// Parallax occlusion mapping: ray-march the depth map in tangent space, refine, optional self-shadow.
#include "geometry/geo_common.glsl"
#include "geometry/pom_params.glsl"
#include "geometry/geo_gbuffer.glsl"
layout(binding = 0) uniform sampler2D tDepthMap;   // r = depth below the top surface (0 top .. 1 deepest)
layout(binding = 1) uniform sampler2D tAlbedo;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vViewTS;
layout(location = 2) in vec3 vLightTS;
layout(location = 3) in vec4 vCurClip;
layout(location = 4) in vec4 vPrevClip;

float depthAt(vec2 uv, float lod){ return textureLod(tDepthMap, uv, lod).r; }

vec2 pomUV(vec2 uv, vec3 V, float lod, out float hitDepth){
    float layers = mix(pP0.w, pP0.z, clamp(abs(V.z), 0.0, 1.0));   // more layers at grazing angles
    float stepD = 1.0 / layers, cur = 0.0;
    vec2 dUV = V.xy / max(V.z, 0.08) * pP0.y / layers;
    float d = depthAt(uv, lod);
    for (int i = 0; i < 96; ++i){ if (cur >= d) break; uv -= dUV; d = depthAt(uv, lod); cur += stepD; }
    vec2 prev = uv + dUV;
    if (pP1.x > 0.5){                                // binary refinement (relief mapping)
        vec2 a = prev, b = uv; float ca = cur - stepD, cb = cur;
        for (int i = 0; i < int(pP1.x); ++i){
            vec2 m = 0.5 * (a + b); float cm = 0.5 * (ca + cb);
            if (depthAt(m, lod) > cm){ a = m; ca = cm; } else { b = m; cb = cm; }
        }
        hitDepth = cb; return b;
    }
    float after = d - cur, before = depthAt(prev, lod) - cur + stepD;   // linear interpolation (POM)
    float w = after / (after - before);
    hitDepth = cur - stepD * (1.0 - w);
    return mix(uv, prev, w);
}
float selfShadow(vec2 uv, float depth, vec3 L, float lod){
    int steps = int(pP1.y);
    if (steps <= 0 || L.z <= 0.0) return 1.0;
    float stepD = depth / float(steps);
    vec2 dUV = L.xy / max(L.z, 0.08) * pP0.y * stepD;
    float cur = depth, s = 0.0;
    for (int i = 0; i < 32; ++i){
        if (i >= steps) break;
        uv += dUV; cur -= stepD;
        s = max(s, (cur - depthAt(uv, lod)) * 6.0);
    }
    return 1.0 - clamp(s, 0.0, 0.8);
}
void main(){
    vec3 V = normalize(vViewTS);
    // mip level from the ORIGINAL uv derivatives (derivatives are undefined inside the divergent loop)
    vec2 sz = vec2(textureSize(tDepthMap, 0));
    vec2 dx = dFdx(vUV) * sz, dy = dFdy(vUV) * sz;
    float lod = max(0.5 * log2(max(dot(dx, dx), dot(dy, dy))), 0.0);
    float hd = 0.0; vec2 uv = vUV;
    if (pP0.x > 0.5) uv = pomUV(vUV, V, lod, hd);
    vec2 tx = 1.0 / sz;
    float l = depthAt(uv - vec2(tx.x, 0), lod), r = depthAt(uv + vec2(tx.x, 0), lod);
    float u = depthAt(uv - vec2(0, tx.y), lod), dn = depthAt(uv + vec2(0, tx.y), lod);
    vec3 nTS = normalize(vec3((r - l) * 3.0, (dn - u) * 3.0, 1.0));      // depth gradient -> normal (normal-map path)
    vec3 N = normalize(pT.xyz * nTS.x + pB.xyz * nTS.y + pN.xyz * nTS.z);
    vec3 albedo = textureLod(tAlbedo, uv, lod).rgb;
    // self-shadow mostly blocks the sun, but the G-buffer has no per-pixel sun mask we may write,
    // so darken albedo (affects direct light) and store cavity occlusion as material AO (ambient)
    float shadow = (pP0.x > 0.5) ? selfShadow(uv, hd, normalize(vLightTS), lod) : 1.0;
    albedo *= mix(0.45, 1.0, shadow);
    float ao = 1.0 - 0.5 * depthAt(uv, lod);
    writeGBuffer(albedo, N, 0.75, 0.0, ao, vec3(0.0), GEO_SHADING_DEFAULT, vCurClip, vPrevClip);
}
