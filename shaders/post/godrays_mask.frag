#version 450
// PostOutput god rays 1/2: sky occlusion mask at half res. See docs/god_rays.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uDepth;
void main() {
    bool sky = textureLod(uDepth, vUV, 0.0).r <= 1e-7;           // reversed-Z: infinity = 0
    vec3 c = sky ? min(textureLod(uSrc, vUV, 0.0).rgb, vec3(4.0)) * 0.25 : vec3(0);
    vec2 dv = (vUV - pSun.xy) * vec2(pSun.w, 1.0);
    if (sky) c += pSunRadiance.rgb * smoothstep(pRays2.y, 0.0, length(dv));
    oColor = vec4(c, 1);
}
