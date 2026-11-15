#version 450
// Tonemap-aware MSAA resolve: weights each sample by 1/(1+max) so bright HDR edges stay smooth.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2DMS uColorMS;
layout(std140, binding = 3) uniform MsaaParams { int uSamples; int uTonemapAware; int _p0; int _p1; };
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 sum = vec3(0); float wsum = 0.0;
    for (int i = 0; i < uSamples; i++) {
        vec3 c = texelFetch(uColorMS, p, i).rgb;
        float w = uTonemapAware != 0 ? 1.0 / (1.0 + max(c.r, max(c.g, c.b))) : 1.0;
        sum += c * w; wsum += w;
    }
    oColor = vec4(sum / wsum, 1.0);
}
