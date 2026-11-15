#ifndef GFX_SKY_GLSL
#define GFX_SKY_GLSL
#include "common/frame.glsl"
// Simple analytic sky used by core paths (lighting modules may draw their own).
vec3 skyColor(vec3 dir) {
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = mix(vec3(0.75, 0.8, 0.9), vec3(0.25, 0.45, 0.85), t);
    float sun = pow(max(dot(dir, normalize(uSunDirection.xyz)), 0.0), 800.0);
    return sky * 1.5 + uSunColor.rgb * sun * 40.0;
}
vec3 viewDirFromUv(vec2 uv) {
    vec3 p = reconstructWorldPos(uv, 1e-4);   // a point far along the view ray (reversed-Z: tiny depth = far)
    return normalize(p - uCameraPos.xyz);
}
// Velocity of a point at infinity: only camera rotation moves it.
vec2 skyVelocity(vec3 dir) {
    vec4 cur = uUnjitteredViewProj * vec4(dir, 0.0);
    vec4 prev = uPrevViewProj * vec4(dir, 0.0);
    return cur.xy / cur.w - prev.xy / prev.w;
}
#endif
