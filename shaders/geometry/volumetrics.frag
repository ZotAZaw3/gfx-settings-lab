#version 450
// Ray-marched volumetric height fog + volumetric cloud slab. Output: rgb in-scattered light,
// a = transmittance; composited as scene = scene * a + rgb. Runs at full/half/quarter resolution.
// Sun visibility inside the fog comes from the showcase terrain heightfield (hills shadow the
// valleys -> light shafts); the lighting thread's cascades are not exposed to Translucent passes.
#include "geometry/geo_common.glsl"
layout(std140, binding = 3) uniform VolumeUBO {
    vec4 vA;   // x fog steps (0 = analytic), y density, z height falloff, w max distance
    vec4 vB;   // x anisotropy g, y terrain shadows, z cloud steps (0 = off), w cloud light steps
    vec4 vC;   // x coverage, y cloud bottom, z cloud top, w terrain displacement (m)
    vec4 vD;   // xy terrain center xz, z terrain size, w unused
};
layout(binding = 0) uniform sampler2D tSceneDepth;   // full res or downsampled (max) depth
layout(binding = 1) uniform sampler2D tHeightMap;    // showcase terrain heightfield
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outFog;

const vec3 kAmbient = vec3(0.10, 0.13, 0.18);
float hg(float c, float g){ float g2 = g * g; return (1.0 - g2) / (12.566 * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5)); }
float terrainH(vec2 xz){
    vec2 uv = (xz - vD.xy) / vD.z + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return -1e3;
    return textureLod(tHeightMap, uv, 0.0).r * vC.w;
}
float sunVisibility(vec3 p){
    if (vB.y < 0.5) return 1.0;
    vec3 L = uSunDirection.xyz;
    for (int i = 1; i <= 6; ++i){                       // short march towards the sun over the heightfield
        vec3 q = p + L * (float(i * i) * 1.5);
        if (q.y < terrainH(q.xz)) return 0.0;
    }
    return 1.0;
}
float fogDensityAt(vec3 p){
    float n = 0.55 + 0.9 * valueNoise(p.xz * 0.04 + vec2(uTime.x * 0.05, 0.0));
    return vA.y * exp(-max(p.y, 0.0) * vA.z) * n;
}
float cloudDensity(vec3 p){
    float h = (p.y - vC.y) / (vC.z - vC.y);
    if (h < 0.0 || h > 1.0) return 0.0;
    float shape = fbm(p.xz * 0.0009 + vec2(uTime.x * 0.004, 0.0));
    float grad = smoothstep(0.0, 0.15, h) * smoothstep(1.0, 0.55, h);
    float d = clamp((shape - (1.0 - vC.x)) * 3.0, 0.0, 1.0) * grad;
    if (d > 0.0) d -= valueNoise(p.xz * 0.012 + p.y * 0.01) * 0.25;   // erosion detail
    return max(d, 0.0) * 0.03;
}
void main(){
    vec2 uv = vUV;
    float depth = textureLod(tSceneDepth, uv, 0.0).r;
    bool sky = depth <= 1e-6;
    vec3 wp = reconstructWorldPos(uv, max(depth, 1e-6));
    vec3 cam = uCameraPos.xyz;
    vec3 rd = normalize(wp - cam);
    float sceneDist = sky ? 1e9 : length(wp - cam);
    float jitter = bayer4(ivec2(gl_FragCoord.xy) + ivec2(int(uTime.z) * 3, int(uTime.z) * 7));
    float cosT = dot(rd, uSunDirection.xyz);
    vec3 sun = uSunColor.rgb * uSunDirection.w * 0.15;
    vec3 L = vec3(0.0); float T = 1.0;
    float dist = min(sceneDist, vA.w);

    if (vA.x < 0.5){
        // analytic exponential height fog: closed-form optical depth along the ray, no shadows
        float k = vA.z, od;
        float base = vA.y * 0.8 * exp(-max(cam.y, 0.0) * k);
        if (abs(rd.y) > 1e-3) od = base * (1.0 - exp(-k * rd.y * dist)) / (k * rd.y);
        else od = base * dist;
        T = exp(-max(od, 0.0));
        L = (kAmbient + sun * hg(cosT, vB.x)) * (1.0 - T);
    } else {
        int steps = int(vA.x);
        float stepLen = dist / float(steps), ph = hg(cosT, vB.x);
        for (int i = 0; i < 128; ++i){
            if (i >= steps) break;
            vec3 p = cam + rd * (float(i) + jitter) * stepLen;
            float s = fogDensityAt(p);
            if (s <= 1e-7) continue;
            vec3 S = (sun * sunVisibility(p) * ph + kAmbient) * s;
            float Ts = exp(-s * stepLen);
            L += T * (S - S * Ts) / s;                  // energy-conserving integration (Hillaire 2015)
            T *= Ts;
            if (T < 0.01) break;
        }
    }
    if (vB.z > 0.5 && sky && rd.y > 0.02){
        float t0 = (vC.y - cam.y) / rd.y, t1 = (vC.z - cam.y) / rd.y;
        int steps = int(vB.z), lsteps = int(vB.w);
        float stepLen = (t1 - t0) / float(steps);
        float ph = mix(hg(cosT, 0.7), hg(cosT, -0.2), 0.3);
        for (int i = 0; i < 256; ++i){
            if (i >= steps || T < 0.01) break;
            vec3 p = cam + rd * (t0 + (float(i) + jitter) * stepLen);
            float s = cloudDensity(p);
            if (s <= 0.0) continue;
            float od = 0.0, ls = (vC.z - vC.y) * 0.35 / float(max(lsteps, 1));
            for (int j = 1; j <= 8; ++j){ if (j > lsteps) break; od += cloudDensity(p + uSunDirection.xyz * ls * float(j)) * ls; }
            float beer = exp(-od), powder = 1.0 - exp(-od * 2.0);
            vec3 S = (sun * 8.0 * beer * mix(1.0, powder, 0.5) * ph + kAmbient * 3.0) * s;
            float Ts = exp(-s * stepLen);
            L += T * (S - S * Ts) / s;
            T *= Ts;
        }
    }
    outFog = vec4(L, T);
}
