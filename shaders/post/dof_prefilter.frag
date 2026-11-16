#version 450
// PostRender DoF 1/4: downsample to half res; rgb + signed CoC in half-res pixels (- near, + far). See docs/depth_of_field.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 1) uniform sampler2D uDepth;
layout(binding = 2) uniform sampler2D uFocus;   // 1x1 focus distance (m)
float coc(float d, float focusM) {
    float focal = pDof.x, fstop = pDof.y, sensor = pDof.z, maxCoC = pDof.w;
    float z = linearDepth(d) * 1000.0;                 // mm
    float F = max(focusM * 1000.0, focal + 1.0);
    float A = focal / fstop;                           // aperture diameter (mm)
    float c = A * focal * (z - F) / (z * (F - focal)); // thin lens CoC on the sensor (mm)
    c = c / sensor * uRenderSize.y * 0.5;              // -> half-res pixels
    return clamp(c, -maxCoC, maxCoC);
}
void main() {
    float focusM = textureLod(uFocus, vec2(0.5), 0.0).r;
    vec2 t = uRenderSize.zw;
    vec2 o[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
    vec3 acc = vec3(0); float wsum = 0.0, cmin = 1e9, cAbsMax = 0.0, cPick = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 uv = vUV + o[i] * t;
        vec3 c = textureLod(uSrc, uv, 0.0).rgb;
        float k = coc(textureLod(uDepth, uv, 0.0).r, focusM);
        float w = (abs(k) + 0.1) / (1.0 + luma(c));    // |CoC| weight (no sharp bleed) + Karis (no fireflies)
        acc += c * w; wsum += w;
        if (abs(k) > cAbsMax) { cAbsMax = abs(k); cPick = k; }
        cmin = min(cmin, k);
    }
    oColor = vec4(acc / max(wsum, 1e-5), cmin < 0.0 ? cmin : cPick); // near field wins so it spreads outward
}
