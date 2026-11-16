#version 450
// Premultiplied output: rgb = emitted/scattered light, a = coverage (0 for additive sparks/fire).
// The effects target may be half/quarter res, so depth testing and soft particles are done here
// against a (downsampled) scene depth instead of with a depth attachment.
#include "geometry/geo_common.glsl"
#include "geometry/particle_common.glsl"
layout(binding = 0) uniform sampler2D tSceneDepth;
layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec3 vColor;
layout(location = 2) in float vT;
layout(location = 0) out vec4 outColor;

float puff(vec2 c, float frame){
    // procedural flipbook: 16 "frames" of noise; with blending we cross-fade frame N and N+1
    float f0 = floor(frame), fr = frame - f0;
    float n0 = valueNoise(c * 2.5 + f0 * 3.7), n1 = valueNoise(c * 2.5 + (f0 + 1.0) * 3.7);
    float n = (eD.x > 0.5) ? mix(n0, n1, fr) : n0;
    return clamp(1.0 - dot(c, c), 0.0, 1.0) * (0.45 + 0.55 * n);
}
void main(){
    float r2 = dot(vCorner, vCorner);
    if (r2 > 1.0) discard;
    int type = int(eA.w);
    float sd = texture(tSceneDepth, gl_FragCoord.xy * eD.zw).r;
    float sceneLin = linearDepth(sd), partLin = linearDepth(gl_FragCoord.z);
    if (partLin > sceneLin) discard;                       // manual depth test (behind opaque geometry)
    float a = (type == 0) ? exp(-r2 * 5.0) : puff(vCorner, vT * 16.0);
    if (type != 0) a *= smoothstep(0.0, 0.1, vT) * (1.0 - smoothstep(0.6, 1.0, vT));
    if (eC.y > 0.5) a *= clamp((sceneLin - partLin) / max(eC.z, 1e-3), 0.0, 1.0);   // soft particles
    vec3 col = vColor;
    if (type == 2) col = mix(vec3(5.0, 2.0, 0.4), vec3(0.8, 0.12, 0.02), vT);
    if (type == 1){
        if (eC.w > 0.5){
            // lit smoke: fake sphere normal in view space, sun + sky ambient
            vec3 nV = normalize(vec3(vCorner.x, vCorner.y, sqrt(max(1.0 - r2, 0.0))));
            vec3 nW = normalize(mat3(uInvView) * nV);
            float ndl = 0.35 + 0.65 * max(dot(nW, uSunDirection.xyz), 0.0);
            col *= uSunColor.rgb * uSunDirection.w * 0.25 * ndl + vec3(0.25, 0.3, 0.38);
        } else col *= 0.8;
        outColor = vec4(col * a, a);
    } else {
        outColor = vec4(col * a, 0.0);
    }
}
