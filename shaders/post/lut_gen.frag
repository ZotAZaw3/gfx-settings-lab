#version 450
// Builds one Z slice of the 32^3 color grading LUT (pPass.x = slice, pPass.y = LUT size). See docs/color_grading_lut.md
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
vec3 whiteBalanceCoeffs(float temp, float tint) {   // simplified Bradford/von Kries in LMS space
    float t1 = temp * 10.0 / 6.0, t2 = tint * 10.0 / 6.0;
    float x = 0.31271 - t1 * (t1 < 0.0 ? 0.1 : 0.05);
    float y = 2.87 * x - 3.0 * x * x - 0.27509507 + t2 * 0.05;
    float X = x / y, Z = (1.0 - x - y) / y;
    vec3 w1 = vec3(0.949237, 1.03542, 1.08728);
    vec3 w2 = vec3(0.7328 * X + 0.4296 - 0.1624 * Z, -0.7036 * X + 1.6975 + 0.0061 * Z, 0.0030 * X + 0.0136 + 0.9834 * Z);
    return w1 / w2;
}
const mat3 LIN_2_LMS = mat3(3.90405e-1, 7.08416e-2, 2.31082e-2, 5.49941e-1, 9.63172e-1, 1.28021e-1, 8.92632e-3, 1.35775e-3, 9.36245e-1);
const mat3 LMS_2_LIN = mat3(2.85847e+0, -2.10182e-1, -4.18120e-2, -1.62879e+0, 1.15820e+0, -1.18169e-1, -2.48910e-2, 3.24281e-4, 1.06867e+0);
vec3 hueRotate(vec3 c, float h) {
    const vec3 k = vec3(0.57735);
    float ca = cos(h), sa = sin(h);
    return c * ca + cross(k, c) * sa + k * dot(k, c) * (1.0 - ca);
}
void main() {
    float N = pPass.y;
    vec3 c = vec3(floor(gl_FragCoord.xy), pPass.x) / (N - 1.0);    // texel centres -> 0..1
    c = LMS_2_LIN * ((LIN_2_LMS * c) * whiteBalanceCoeffs(pGrade.x, pGrade.y));
    const float mid = -2.4739312;                                  // log2(0.18)
    c = exp2((log2(max(c, vec3(1e-5))) - mid) * pGrade.z + mid);    // contrast around middle grey
    c = c * pGain.rgb + pLift.rgb * (1.0 - c);                     // lift / gain
    c = pow(max(c, vec3(0)), 1.0 / max(pGamma.rgb, vec3(0.01)));   // gamma (midtones)
    c = hueRotate(c, pGrade2.y);
    float l = luma(c);
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = mix(vec3(l), c, 1.0 + pGrade2.x * (1.0 - sat));            // vibrance: boosts dull colors more
    c = mix(vec3(l), c, pGrade.w);                                 // saturation
    oColor = vec4(clamp(c, 0.0, 1.0), 1);
}
