// geometry/water_common.glsl - mirrors WaterParams in water.cpp (std140, UBO 3) + Gerstner waves
layout(std140, binding = 3) uniform WaterUBO {
    vec4 wA;   // x wave count (0 = flat + normal noise), y refraction, z screen-space reflections, w foam
    vec4 wB;   // x caustics, y water level, z grid size (m), w unused
    vec4 wC;   // xy center xz, zw unused
};
// Gerstner wave: points move in circles -> sharp crests, flat troughs. Returns displacement, accumulates tangents.
vec3 gerstner(vec2 xz, vec2 dir, float steep, float wavelength, float t, inout vec3 T, inout vec3 B){
    float k = 6.2831853 / wavelength, c = sqrt(9.8 / k), f = k * (dot(dir, xz) - c * t);
    float a = steep / k;
    T += vec3(-dir.x * dir.x * steep * sin(f), dir.x * steep * cos(f), -dir.x * dir.y * steep * sin(f));
    B += vec3(-dir.x * dir.y * steep * sin(f), dir.y * steep * cos(f), -dir.y * dir.y * steep * sin(f));
    return vec3(dir.x * a * cos(f), a * sin(f), dir.y * a * cos(f));
}
vec3 waveSum(vec2 xz, float t, int count, out vec3 N){
    vec3 T = vec3(1, 0, 0), B = vec3(0, 0, 1), d = vec3(0.0);
    for (int i = 0; i < 12; ++i){
        if (i >= count) break;
        float fi = float(i);
        float ang = fi * 2.399 + 0.3;
        vec2 dir = vec2(cos(ang), sin(ang));
        float wl = 9.0 * pow(0.72, fi) + 0.6;
        d += gerstner(xz, dir, 0.22 / (1.0 + fi * 0.35), wl, t, T, B);
    }
    N = normalize(cross(B, T));
    return d;
}
