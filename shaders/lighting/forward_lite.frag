#version 450
// forward_lite.frag - shades with the shared shadeSurface(). Screen-space inputs (AO, SSR, SSGI,
// contact shadows) are bound as neutral 1x1 fallbacks for these passes, because the screen
// positions of a mirrored or cube-face camera don't match the main view.
#include "common/draw.glsl"
#include "lighting/shade.glsl"
layout(std140, binding = 3) uniform ForwardLitePass {
    vec4 uClipPlane;    // world plane (n.xyz, d): fragments with dot(n,p)+d < 0 are clipped (planar)
    vec4 uEyePos;       // xyz eye of this pass (mirrored camera / probe center)
};
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 0) out vec4 oColor;
void main() {
    if (dot(uClipPlane.xyz, vWorldPos) + uClipPlane.w < 0.0) discard;
    GpuMaterial m = materials[uIds.x];
    vec4 base = m.baseColor * texture(tBaseColor, vUV);
    if (m.tex1.z == 1 && base.a < m.params.w) discard;           // alpha mask
    vec4 mr = texture(tMetalRough, vUV);                         // glTF: g roughness, b metallic
    Surface s;
    s.position = vWorldPos;
    s.normal = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    s.albedo = base.rgb;
    s.roughness = clamp(m.emissiveRough.a * mr.g, 0.03, 1.0);
    s.metallic = m.params.x * mr.b;
    s.ao = mix(1.0, texture(tOcclusion, vUV).r, m.params.z);
    s.emissive = m.emissiveRough.rgb * texture(tEmissive, vUV).rgb;
    s.shadingModel = m.tex1.w;
    s.sssMask = m.sss.a > 0.0 ? 1.0 : 0.0;
    vec3 V = normalize(uEyePos.xyz - vWorldPos);
    // uv is only used for screen-space inputs, which are neutral here
    oColor = vec4(shadeSurface(s, V, vec2(0.5)), 1.0);
}
