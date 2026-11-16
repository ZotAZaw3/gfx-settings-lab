#version 450
// Water shading: Fresnel mix of refraction (scene colour behind, absorbed by water depth) and
// reflection (screen-space ray march, sky fallback), crest/shore foam, caustics on the bottom.
#include "geometry/geo_common.glsl"
#include "geometry/water_common.glsl"
layout(binding = 0) uniform sampler2D tSceneDepth;
layout(binding = 1) uniform sampler2D tSceneColor;   // copy of scene.color (we write scene.color)
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vCrest;
layout(location = 0) out vec4 outColor;

vec3 skyColor(vec3 d){
    vec3 sky = mix(vec3(0.55, 0.62, 0.70), vec3(0.20, 0.35, 0.60), clamp(d.y * 2.0, 0.0, 1.0));
    return sky * uSunDirection.w * 0.25;
}
void main(){
    vec2 uv = gl_FragCoord.xy * uRenderSize.zw;
    float sceneD = texelFetch(tSceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float sceneLin = linearDepth(sceneD), waterLin = linearDepth(gl_FragCoord.z);
    if (sceneD > 1e-7 && waterLin > sceneLin) discard;                  // manual depth test (scene in front)
    vec3 V = normalize(uCameraPos.xyz - vWorld);
    // small ripples on top of the big waves (the only waves on Low)
    vec2 rp = vWorld.xz * 1.3;
    vec3 ripple = vec3(valueNoise(rp + uTime.x * 0.6) - valueNoise(rp + vec2(5.2, 1.3) - uTime.x * 0.5), 0.0,
                       valueNoise(rp.yx + uTime.x * 0.4) - valueNoise(rp.yx + vec2(2.1, 7.7) - uTime.x * 0.7)) * 0.25;
    vec3 N = normalize(vNormal + ripple);
    float F = 0.02 + 0.98 * pow(1.0 - max(dot(N, V), 0.0), 5.0);         // Schlick Fresnel, F0 = 0.02

    // refraction + absorption
    float waterDepth = max(sceneLin - waterLin, 0.0) * (sceneD <= 1e-7 ? 0.0 : 1.0) + (sceneD <= 1e-7 ? 50.0 : 0.0);
    vec3 refr = vec3(0.0);
    if (wA.y > 0.5){
        vec2 ruv = clamp(uv + N.xz * 0.04 * clamp(waterDepth, 0.0, 1.0), vec2(0.001), vec2(0.999));
        if (linearDepth(textureLod(tSceneDepth, ruv, 0.0).r) < waterLin) ruv = uv;   // don't refract things in front
        refr = textureLod(tSceneColor, ruv, 0.0).rgb;
        if (wB.x > 0.5){                                                  // caustics: animated light pattern on the bottom
            vec3 bottom = vWorld - V * waterDepth;
            float c = valueNoise(bottom.xz * 0.9 + uTime.x * 0.4) + valueNoise(bottom.xz * 1.3 - uTime.x * 0.3);
            refr *= 1.0 + pow(abs(1.0 - abs(c - 1.0)), 6.0) * 1.5 * exp(-waterDepth * 0.3);
        }
    }
    vec3 absorb = exp(-vec3(0.45, 0.12, 0.08) * waterDepth);
    vec3 deep = vec3(0.01, 0.05, 0.07) * uSunDirection.w * 0.2;
    vec3 under = refr * absorb + deep * (1.0 - absorb);

    // reflection: screen-space march, fallback to sky + sun glint
    vec3 R = reflect(-V, N);
    vec3 refl = skyColor(R) + uSunColor.rgb * uSunDirection.w * pow(max(dot(R, uSunDirection.xyz), 0.0), 800.0) * 4.0;
    if (wA.z > 0.5){
        vec3 p = vWorld;
        for (int i = 1; i <= 24; ++i){
            p += R * (0.4 * float(i));
            vec4 c = uUnjitteredViewProj * vec4(p, 1.0);
            if (c.w <= 0.0) break;
            vec2 suv = ndcToUv(c.xy / c.w);
            if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
            float sd = textureLod(tSceneDepth, suv, 0.0).r;
            float sl = linearDepth(sd), pl = c.w;
            if (sd > 1e-7 && pl > sl && pl < sl + 2.0){
                float edge = 1.0 - smoothstep(0.85, 1.0, max(abs(suv.x * 2.0 - 1.0), abs(suv.y * 2.0 - 1.0)));
                refl = mix(refl, textureLod(tSceneColor, suv, 0.0).rgb, edge);
                break;
            }
        }
    }
    vec3 col = mix(under, refl, F);
    if (wA.w > 0.5){                                                       // foam: wave crests + shoreline
        float shore = 1.0 - smoothstep(0.0, 0.6, waterDepth);
        float crest = smoothstep(0.35, 0.6, vCrest);
        float pattern = valueNoise(vWorld.xz * 3.0 + uTime.x * 0.3);
        col = mix(col, vec3(0.9) * uSunDirection.w * 0.25, clamp((shore + crest) * pattern * 1.5, 0.0, 0.85));
    }
    outColor = vec4(col, 1.0);
}
