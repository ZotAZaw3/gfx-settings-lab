// shadow_sample.glsl - cascaded shadow map lookup with HW PCF / Poisson PCF / PCSS,
// capsule shadows. See docs/shadows.md. Shadow maps use standard Z (0 near, 1 far).
#ifndef SHADOW_SAMPLE_GLSL
#define SHADOW_SAMPLE_GLSL

layout(binding = 8) uniform sampler2DArrayShadow uShadowCmp;  // compare sampler (LessEqual, standard Z)
layout(binding = 9) uniform sampler2DArray       uShadowRaw;   // same texture, no compare (PCSS blocker search)

const vec2 POISSON16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760),  vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),  vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));

float shadowTap(vec2 uv, float layer, float z) { return texture(uShadowCmp, vec4(uv, layer, z)); }

float pcfGrid3(vec3 c, float layer, vec2 texel) {
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            s += shadowTap(c.xy + vec2(x, y) * texel, layer, c.z);
    return s / 9.0;
}

float pcfPoisson(vec3 c, float layer, vec2 texel, float radius, float rot) {
    float s = 0.0;
    mat2 R = mat2(cos(rot), sin(rot), -sin(rot), cos(rot));
    for (int i = 0; i < 16; ++i)
        s += shadowTap(c.xy + R * POISSON16[i] * texel * radius, layer, c.z);
    return s / 16.0;
}

float pcss(vec3 c, float layer, vec2 texel, float rot, int cascade) {
    mat2 R = mat2(cos(rot), sin(rot), -sin(rot), cos(rot));
    // 1) blocker search
    float searchR = uShadowParams.y * 12.0;
    float sum = 0.0, n = 0.0;
    for (int i = 0; i < 16; ++i) {
        float d = texture(uShadowRaw, vec3(c.xy + R * POISSON16[i] * texel * searchR, layer)).r;
        if (d < c.z) { sum += d; n += 1.0; }
    }
    if (n == 0.0) return 1.0;
    float blocker = sum / n;
    // 2) penumbra width; ortho depth is linear, so the ratio is in normalized depth units
    float penumbra = (c.z - blocker) / max(blocker, 1e-3) * uShadowParams.y * 64.0;
    penumbra = clamp(penumbra, 1.0, 24.0);
    // 3) filter
    return pcfPoisson(c, layer, texel, penumbra, rot);
}

int selectCascade(float viewDepth) {
    int count = uShadowCfg.x;
    for (int i = 0; i < count; ++i)
        if (viewDepth < uCascadeSplits[i]) return i;
    return -1;
}

float sampleCascade(int i, vec3 worldPos, vec3 N, vec2 pixel) {
    vec3 p = worldPos + N * uShadowParams.x * uCascadeTexel[i];   // normal offset
    vec4 lc = uCascadeVP[i] * vec4(p, 1.0);
    vec3 c = lc.xyz / lc.w;
    c.xy = ndcToUv(c.xy);
    if (any(lessThan(c.xy, vec2(0))) || any(greaterThan(c.xy, vec2(1))) || c.z > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowCmp, 0).xy);
    float rot = ign(pixel, frameIndex()) * 6.2831853;
    int f = uShadowCfg.y;
    if (f == 0) return shadowTap(c.xy, float(i), c.z);
    if (f == 1) return pcfGrid3(c, float(i), texel);
    if (f == 2) return pcfPoisson(c, float(i), texel, 2.5, rot);
    return pcss(c, float(i), texel, rot, i);
}

// Visibility of the sun for a world position. viewDepth is positive distance along -Z.
float sunShadow(vec3 worldPos, vec3 N, float viewDepth, vec2 pixel) {
    if (uShadowCfg.x <= 0) return 1.0;
    if (viewDepth > uShadowParams.z) return 1.0;           // beyond shadow distance
    int i = selectCascade(viewDepth);
    if (i < 0) return 1.0;
    float v = sampleCascade(i, worldPos, N, pixel);
    // blend into the next cascade near the split to hide seams
    float splitEnd = uCascadeSplits[i];
    float splitStart = i == 0 ? 0.0 : uCascadeSplits[i - 1];
    float t = (splitEnd - viewDepth) / max(splitEnd - splitStart, 1e-3);
    if (t < uShadowParams.w && i + 1 < uShadowCfg.x) {
        float v2 = sampleCascade(i + 1, worldPos, N, pixel);
        v = mix(v2, v, t / uShadowParams.w);
    }
    // fade out at the max shadow distance
    float fade = clamp((uShadowParams.z - viewDepth) / (uShadowParams.z * 0.1), 0.0, 1.0);
    return mix(1.0, v, fade);
}

// Capsule (character) shadows: soft analytic occlusion from capsules toward the sun.
// Used for characters so they get a soft contact shadow even when shadow maps are low-res.
float capsuleShadow(vec3 p, vec3 L, float coneCos) {
    float vis = 1.0;
    for (int i = 0; i < uShadowCfg.w; ++i) {
        vec3 a = uCapsuleA[i].xyz, b = uCapsuleB[i].xyz;
        float r = uCapsuleA[i].w;
        // closest point on the segment to the ray p + t*L
        vec3 ba = b - a, oa = p - a;
        float baba = dot(ba, ba), bal = dot(ba, L), baoa = dot(ba, oa), loa = dot(L, oa);
        float denom = baba - bal * bal;
        float s = denom > 1e-5 ? clamp((baoa - bal * loa) / denom, 0.0, 1.0) : 0.0;
        vec3 c = a + ba * s;
        vec3 pc = c - p;
        float t = max(dot(pc, L), 0.0);
        float d = length(pc - L * t);
        // angle subtended vs. cone of the light: soft occlusion
        float occ = 1.0 - smoothstep(r * 0.5, r + t * (1.0 - coneCos) * 8.0, d);
        occ *= smoothstep(0.0, r, t);
        vis *= 1.0 - occ * 0.85;
    }
    return vis;
}

#endif
