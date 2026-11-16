// Shared ray tracing code: scene buffers, BVH traversal, RNG and sampling.
// See docs/raytracing_overview.md. Included by every shaders/raytracing/*.comp.
// Conventions: reversed-Z (near=1, far=0, 0 = sky), D3D clip space (z 0..1), uv (0,0) top-left.
#ifndef RT_COMMON_GLSL
#define RT_COMMON_GLSL

#define RT_PI 3.14159265359
#define RT_INF 1e30

// ---------------------------------------------------------------- scene buffers
struct BVHNode { vec4 bminLeft; vec4 bmaxCount; };      // .w are uints stored as float bits
struct Tri     { vec4 v0; vec4 v1; vec4 v2; };
struct TriAttr { vec4 n0; vec4 n1; vec4 n2; };         // n0.w = material index
struct Material { vec4 albedoRoughness; vec4 emissiveMetallic; vec4 transmissionIor; };

layout(std430, binding = 10) readonly buffer RTNodes     { BVHNode rtNodes[]; };
layout(std430, binding = 11) readonly buffer RTTris      { Tri rtTris[]; };
layout(std430, binding = 12) readonly buffer RTAttrs     { TriAttr rtAttrs[]; };
layout(std430, binding = 13) readonly buffer RTMaterials { Material rtMaterials[]; };

layout(std140, binding = 10) uniform RTParams {
    mat4  invViewProj;     // current JITTERED (matches the rasterized G-buffer depth)
    mat4  invViewProjCam;  // current unjittered (path tracer camera rays)
    mat4  prevViewProj;    // previous frame, unjittered (reprojection)
    vec4  camPos;          // xyz, w = frames accumulated (path tracer)
    vec4  sunDir;          // xyz toward the sun (normalized), w = angular radius (radians)
    vec4  sunRadiance;     // rgb * intensity
    vec4  skyZenith;       // rgb
    vec4  skyHorizon;      // rgb
    ivec4 frameInfo;       // x frame index, y rays per pixel, z max bounces, w flags
    vec4  effectParams;    // x AO radius, y reflection roughness cutoff, z max ray distance, w normal bias
    ivec4 sizes;           // xy trace resolution, zw full (G-buffer) resolution
    vec4  denoiseParams;   // x temporal alpha min, y step size (a-trous), z depth tolerance, w sigma luminance
    ivec4 misc;            // x composite mode flags (1 = scalar source), y RR active, zw unused
};

#define RT_FLAG_RUSSIAN_ROULETTE 1
#define RT_FLAG_ACCUMULATE       2
#define RT_FLAG_HISTORY_VALID    4
#define RT_FLAG_FIREFLY_CLAMP    8

// ---------------------------------------------------------------- RNG (PCG hash, per pixel/frame/dimension)
uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
struct Rng { uint s; };
Rng rngInit(uvec2 pixel, uint frame, uint salt) {
    Rng r; r.s = pcg(pixel.x + pcg(pixel.y + pcg(frame * 9781u + salt))); return r;
}
float rand1(inout Rng r) { r.s = pcg(r.s); return float(r.s) * (1.0 / 4294967296.0); }
vec2  rand2(inout Rng r) { return vec2(rand1(r), rand1(r)); }

// ---------------------------------------------------------------- sampling
vec3 tangentToWorld(vec3 v, vec3 n) {
    // Frisvad / Duff et al. 2017 orthonormal basis
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    vec3 t = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    vec3 bt = vec3(b, s + n.y * n.y * a, -n.y);
    return normalize(v.x * t + v.y * bt + v.z * n);
}
// Cosine-weighted hemisphere: pdf = cos(theta)/pi. Used by AO, GI, diffuse bounces.
vec3 sampleCosineHemisphere(vec3 n, vec2 u) {
    float r = sqrt(u.x), phi = 2.0 * RT_PI * u.y;
    return tangentToWorld(vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x))), n);
}
// Uniform direction inside a cone of half-angle `angle` around `axis` (sun disc). Used by soft shadows.
vec3 sampleCone(vec3 axis, float angle, vec2 u) {
    float cosMax = cos(angle);
    float cosT = mix(1.0, cosMax, u.x);
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 2.0 * RT_PI * u.y;
    return tangentToWorld(vec3(sinT * cos(phi), sinT * sin(phi), cosT), axis);
}
// GGX visible-normal sampling (Heitz 2018). v in world space (toward viewer), returns microfacet normal.
vec3 sampleGGXVNDF(vec3 v, vec3 n, float alpha, vec2 u) {
    // Build local frame
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    vec3 t = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    vec3 bt = vec3(b, s + n.y * n.y * a, -n.y);
    vec3 vl = vec3(dot(v, t), dot(v, bt), dot(v, n));
    vec3 vh = normalize(vec3(alpha * vl.x, alpha * vl.y, vl.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1, 0, 0);
    vec3 T2 = cross(vh, T1);
    float r = sqrt(u.x), phi = 2.0 * RT_PI * u.y;
    float t1 = r * cos(phi), t2 = r * sin(phi);
    float sh = 0.5 * (1.0 + vh.z);
    t2 = (1.0 - sh) * sqrt(max(0.0, 1.0 - t1 * t1)) + sh * t2;
    vec3 nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * vh;
    vec3 hl = normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
    return normalize(hl.x * t + hl.y * bt + hl.z * n);
}

vec3 fresnelSchlick(float cosT, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - clamp(cosT, 0.0, 1.0), 5.0); }
float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Smith G1 for GGX (height-correlated pieces), used to weight VNDF samples: weight = F * G2/G1(v)
float smithG1(float NdotX, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * NdotX / (NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX));
}

// ---------------------------------------------------------------- sky
vec3 skyRadiance(vec3 d) {
    float h = clamp(d.y, -1.0, 1.0);
    vec3 sky = mix(skyHorizon.rgb, skyZenith.rgb, pow(max(h, 0.0), 0.5));
    if (h < 0.0) sky = skyHorizon.rgb * 0.3; // ground
    return sky;
}

// ---------------------------------------------------------------- traversal
struct Hit { float t; uint tri; vec2 bary; };

float rayBox(vec3 o, vec3 invD, vec3 bmin, vec3 bmax, float tMax) {
    vec3 t0 = (bmin - o) * invD, t1 = (bmax - o) * invD;
    vec3 tn = min(t0, t1), tf = max(t0, t1);
    float tnear = max(max(tn.x, tn.y), max(tn.z, 0.0));
    float tfar = min(min(tf.x, tf.y), min(tf.z, tMax));
    return tnear <= tfar ? tnear : RT_INF;
}

// Moller-Trumbore. Returns t (or -1) and barycentrics (u,v) of v1,v2.
float rayTri(vec3 o, vec3 d, Tri tri, out vec2 bary) {
    vec3 e1 = tri.v1.xyz - tri.v0.xyz, e2 = tri.v2.xyz - tri.v0.xyz;
    vec3 p = cross(d, e2);
    float det = dot(e1, p);
    bary = vec2(0.0);
    if (abs(det) < 1e-10) return -1.0;
    float inv = 1.0 / det;
    vec3 s = o - tri.v0.xyz;
    float u = dot(s, p) * inv;
    if (u < 0.0 || u > 1.0) return -1.0;
    vec3 q = cross(s, e1);
    float v = dot(d, q) * inv;
    if (v < 0.0 || u + v > 1.0) return -1.0;
    float t = dot(e2, q) * inv;
    bary = vec2(u, v);
    return t > 0.0 ? t : -1.0;
}

#define RT_STACK_SIZE 64

// anyHit = true: stop at the first intersection (shadow / AO rays).
bool rtTrace(vec3 o, vec3 d, float tMax, bool anyHit, out Hit hit) {
    hit.t = tMax; hit.tri = 0xFFFFFFFFu; hit.bary = vec2(0.0);
    vec3 invD = 1.0 / d;
    uint stack[RT_STACK_SIZE];
    int sp = 0;
    uint idx = 0u;
    if (rayBox(o, invD, rtNodes[0].bminLeft.xyz, rtNodes[0].bmaxCount.xyz, hit.t) >= RT_INF) return false;
    for (;;) {
        BVHNode n = rtNodes[idx];
        uint count = floatBitsToUint(n.bmaxCount.w);
        uint left = floatBitsToUint(n.bminLeft.w);
        if (count > 0u) {
            for (uint i = 0u; i < count; ++i) {
                vec2 b;
                float t = rayTri(o, d, rtTris[left + i], b);
                if (t > 0.0 && t < hit.t) {
                    hit.t = t; hit.tri = left + i; hit.bary = b;
                    if (anyHit) return true;
                }
            }
        } else {
            uint a = left, c = left + 1u;
            float da = rayBox(o, invD, rtNodes[a].bminLeft.xyz, rtNodes[a].bmaxCount.xyz, hit.t);
            float dc = rayBox(o, invD, rtNodes[c].bminLeft.xyz, rtNodes[c].bmaxCount.xyz, hit.t);
            if (da > dc) { float tf = da; da = dc; dc = tf; uint ti = a; a = c; c = ti; }
            if (da < RT_INF) {
                if (dc < RT_INF && sp < RT_STACK_SIZE) stack[sp++] = c;
                idx = a;
                continue;
            }
        }
        if (sp == 0) break;
        idx = stack[--sp];
    }
    return hit.tri != 0xFFFFFFFFu;
}

bool traceAny(vec3 o, vec3 d, float tMax) { Hit h; return rtTrace(o, d, tMax, true, h); }
bool traceClosest(vec3 o, vec3 d, float tMax, out Hit h) { return rtTrace(o, d, tMax, false, h); }

// Surface data at a hit
struct Surface { vec3 pos; vec3 n; vec3 albedo; float roughness; float metallic; vec3 emissive; float transmission; float ior; };

Surface surfaceAt(vec3 o, vec3 d, Hit h) {
    TriAttr a = rtAttrs[h.tri];
    float w = 1.0 - h.bary.x - h.bary.y;
    Surface s;
    s.pos = o + d * h.t;
    s.n = normalize(a.n0.xyz * w + a.n1.xyz * h.bary.x + a.n2.xyz * h.bary.y);
    if (dot(s.n, d) > 0.0) s.n = -s.n; // two-sided
    Material m = rtMaterials[uint(a.n0.w)];
    s.albedo = m.albedoRoughness.rgb;
    s.roughness = clamp(m.albedoRoughness.a, 0.02, 1.0);
    s.metallic = m.emissiveMetallic.a;
    s.emissive = m.emissiveMetallic.rgb;
    s.transmission = m.transmissionIor.x;
    s.ior = max(m.transmissionIor.y, 1.0);
    return s;
}

// Geometric (face) normal, for robust ray offsets
vec3 faceNormal(uint tri) {
    Tri t = rtTris[tri];
    return normalize(cross(t.v1.xyz - t.v0.xyz, t.v2.xyz - t.v0.xyz));
}

// Offset a ray origin off a surface, scaled with distance from the camera to fight float error.
vec3 offsetOrigin(vec3 p, vec3 n) {
    float scale = effectParams.w * (1.0 + length(p - camPos.xyz) * 0.01);
    return p + n * scale;
}

// Direct sun light at a surface (Lambert + simple GGX spec) with an RT shadow ray.
vec3 shadeSunAt(Surface s, vec3 V, inout Rng rng) {
    vec3 L = sampleCone(sunDir.xyz, sunDir.w, rand2(rng));
    float NdotL = dot(s.n, L);
    if (NdotL <= 0.0) return vec3(0.0);
    if (traceAny(offsetOrigin(s.pos, s.n), L, RT_INF)) return vec3(0.0);
    vec3 diffuse = s.albedo * (1.0 - s.metallic) / RT_PI;
    // cheap spec at hits: normalized Blinn-Phong equivalent of the roughness
    vec3 H = normalize(V + L);
    float a2 = s.roughness * s.roughness; a2 *= a2;
    float NdotH = max(dot(s.n, H), 0.0);
    float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (RT_PI * dd * dd);
    vec3 F0 = mix(vec3(0.04), s.albedo, s.metallic);
    vec3 spec = fresnelSchlick(max(dot(H, V), 0.0), F0) * D * 0.25;
    return (diffuse + spec) * sunRadiance.rgb * NdotL;
}

#endif
