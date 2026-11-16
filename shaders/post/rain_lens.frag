#version 450
// PostOutput: rain drops on the lens (procedural, no textures). See docs/screen_rain_wet.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uBlur;   // a wide bloom mip = fogged lens
vec3 staticDrops(vec2 uv, float scale, float t, uint seed) {
    float aspect = pSun.w, amount = pRain.x, size = pRain.y;
    vec2 g = uv * vec2(aspect, 1.0) * scale;
    vec2 id = floor(g), f = fract(g) - 0.5;
    vec3 r = hash33(uvec3(uvec2(ivec2(id) + 4096), seed));
    if (r.z > amount) return vec3(0);
    float life = fract(t * 0.05 + r.x);               // drops appear, then evaporate
    vec2 c = (r.xy - 0.5) * 0.6;
    float rad = mix(0.12, 0.3, r.y) * size * (1.0 - life * 0.5);
    float m = smoothstep(rad, rad * 0.75, length(f - c)) * smoothstep(1.0, 0.8, life);
    return vec3((f - c) / max(rad, 1e-3) * m, m);
}
vec3 streakDrops(vec2 uv, float scale, float t) {
    float aspect = pSun.w, amount = pRain.x, size = pRain.y, speed = pRain.z;
    vec2 g = uv * vec2(aspect, 1.0) * scale; g.x *= 0.5;
    float col = floor(g.x);
    vec3 r = hash33(uvec3(uint(int(col) + 4096), 7u, 3u));
    if (r.z > amount * 0.6) return vec3(0);
    g.y -= t * speed * (0.5 + r.x) + r.y * 10.0;      // slides down the screen (uv.y grows downward)
    vec2 id = floor(g), f = fract(g) - 0.5;
    vec2 c = vec2(sin(id.y * 3.0 + g.y * 2.0) * 0.15 * r.y, 0.0);
    vec2 q = (f - c) * vec2(2.0, 1.0);
    float rad = 0.22 * size;
    float m = smoothstep(rad, rad * 0.75, length(q * vec2(1.0, 1.3)));
    float trail = smoothstep(0.08, 0.0, abs(f.x - c.x)) * smoothstep(-0.5, 0.1, f.y) * smoothstep(0.5, 0.0, f.y);
    float tiny = step(0.75, fract(f.y * 8.0)) * trail * 0.5;   // droplets left behind (above the drop)
    return vec3(q / max(rad, 1e-3) * m, max(m, tiny));
}
void main() {
    float t = frameTime(); int layers = int(pRain2.x);
    vec3 acc = staticDrops(vUV, 9.0, t, 1u);
    if (layers > 1) acc += staticDrops(vUV, 17.0, t + 3.0, 2u) * 0.8;
    if (layers > 2) acc += streakDrops(vUV, 5.0, t);
    float m = clamp(acc.z, 0.0, 1.0);
    vec2 n = acc.xy;
    vec3 sharp = textureLod(uSrc, vUV - n * 0.035, 0.0).rgb;   // refracted (slightly flipped) view through the drop
    vec3 base = mix(textureLod(uSrc, vUV, 0.0).rgb, textureLod(uBlur, vUV, 0.0).rgb, pRain.w * pRain.x);
    vec3 c = mix(base, sharp, m);
    c += m * pow(max(dot(normalize(vec3(n, 0.6)), normalize(vec3(-0.5, -0.6, 0.6))), 0.0), 40.0) * 0.6;
    oColor = vec4(c, 1);
}
