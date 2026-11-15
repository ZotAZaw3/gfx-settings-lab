#version 450
// Core fallback deferred resolve, used only when no lighting module is compiled in.
#include "common/frame.glsl"
#include "lighting/shade.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(binding = 0) uniform sampler2D tAlbedo;
layout(binding = 1) uniform sampler2D tNormal;
layout(binding = 2) uniform sampler2D tMaterial;
layout(binding = 3) uniform sampler2D tEmissive;
layout(binding = 4) uniform sampler2D tDepth;

vec3 skyColor(vec3 dir) {
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = mix(vec3(0.75, 0.8, 0.9), vec3(0.25, 0.45, 0.85), t);
    float sun = pow(max(dot(dir, normalize(uSunDirection.xyz)), 0.0), 800.0);
    return sky * 1.5 + uSunColor.rgb * sun * 40.0;
}

void main() {
    float depth = texture(tDepth, vUV).r;
    vec3 world = reconstructWorldPos(vUV, max(depth, 1e-7));
    vec3 V = normalize(uCameraPos.xyz - world);
    if (depth <= 0.0) { oColor = vec4(skyColor(-V), 1.0); return; }
    Surface s;
    vec4 n = texture(tNormal, vUV);
    vec4 m = texture(tMaterial, vUV);
    vec4 e = texture(tEmissive, vUV);
    s.position = world;
    s.normal = normalize(n.xyz);
    s.albedo = texture(tAlbedo, vUV).rgb;
    s.roughness = m.r; s.metallic = m.g; s.ao = m.b;
    s.emissive = e.rgb;
    s.shadingModel = int(n.w * 255.0 + 0.5);
    s.sssMask = e.a;
    oColor = vec4(shadeSurface(s, V, vUV), 1.0);
}
