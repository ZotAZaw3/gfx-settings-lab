// Shared helpers for TAA, the temporal upscaler, the denoiser and frame generation.
// Engine conventions: D3D-style clip space (uv 0,0 = top-left, NDC y up), reversed-Z depth,
// scene.velocity = currentNDC - previousNDC (RG16F).
#ifndef AA_COMMON_TEMPORAL
#define AA_COMMON_TEMPORAL

vec2 velocityToUV(vec2 ndcDelta) { return ndcDelta * vec2(0.5, -0.5); }

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 rgbToYCoCg(vec3 c) {
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}
vec3 yCoCgToRgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Reversible tonemap used to weight HDR samples so highlights don't dominate blends.
vec3 tmWeight(vec3 c)   { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 tmUnweight(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

// Closest-depth (reversed-Z: largest value) offset in a 3x3 neighbourhood, in texels.
ivec2 closestDepthOffset(sampler2D depthTex, ivec2 p) {
    ivec2 best = ivec2(0); float bd = -1.0;
    ivec2 lim = textureSize(depthTex, 0) - 1;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        float d = texelFetch(depthTex, clamp(p + ivec2(x, y), ivec2(0), lim), 0).r;
        if (d > bd) { bd = d; best = ivec2(x, y); }
    }
    return best;
}

// 5-tap Catmull-Rom history sample (Jimenez / Pettineo approximation).
vec3 sampleCatmullRom(sampler2D tex, vec2 uv) {
    vec2 texSize = vec2(textureSize(tex, 0));
    vec2 samplePos = uv * texSize;
    vec2 tc1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - tc1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc0 = (tc1 - 1.0) / texSize;
    vec2 tc3 = (tc1 + 2.0) / texSize;
    vec2 tc12 = (tc1 + w2 / w12) / texSize;
    vec3 r = vec3(0.0);
    r += textureLod(tex, vec2(tc12.x, tc0.y), 0.0).rgb * (w12.x * w0.y);
    r += textureLod(tex, vec2(tc0.x, tc12.y), 0.0).rgb * (w0.x * w12.y);
    r += textureLod(tex, vec2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y);
    r += textureLod(tex, vec2(tc3.x, tc12.y), 0.0).rgb * (w3.x * w12.y);
    r += textureLod(tex, vec2(tc12.x, tc3.y), 0.0).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, vec3(0.0));
}

// Clip a colour toward the box centre (Playdead / Salvi): less flicker than a hard clamp.
vec3 clipToAABB(vec3 hist, vec3 lo, vec3 hi) {
    vec3 c = 0.5 * (hi + lo), e = 0.5 * (hi - lo) + 1e-5;
    vec3 v = hist - c;
    vec3 a = abs(v / e);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? c + v / m : hist;
}

bool outsideUV(vec2 uv) { return any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))); }

#endif
