#version 450
// SMAA pass 1: luma edge detection with local contrast adaptation. Output RG8 (R = left edge, G = top edge).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec2 oEdges;
layout(binding = 0) uniform sampler2D uColor;    // LDR, gamma space
layout(std140, binding = 3) uniform SmaaParams { vec4 uRcpSize; /* xy 1/size, z range (1 SDR) */ float uThreshold; int uMaxSearch; int uDiag; float uContrastFactor; };
// linear display-referred input -> perceptual luma (sqrt), normalised by uRange for HDR output
float L(ivec2 p) { ivec2 lim = textureSize(uColor, 0) - 1; return sqrt(max(dot(texelFetch(uColor, clamp(p, ivec2(0), lim), 0).rgb / uRcpSize.z, vec3(0.2126, 0.7152, 0.0722)), 0.0)); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float c = L(p), l = L(p + ivec2(-1, 0)), t = L(p + ivec2(0, -1));
    vec2 delta = abs(c - vec2(l, t));
    vec2 edges = step(uThreshold, delta);
    if (dot(edges, vec2(1)) == 0.0) discard;
    float r = L(p + ivec2(1, 0)), b = L(p + ivec2(0, 1));
    float l2 = L(p + ivec2(-2, 0)), t2 = L(p + ivec2(0, -2));
    vec2 dMax = max(delta, abs(c - vec2(r, b)));
    dMax = max(dMax, abs(vec2(l, t) - vec2(l2, t2)));
    float finalMax = max(dMax.x, dMax.y);
    edges *= step(finalMax, uContrastFactor * delta);   // local contrast adaptation (factor 2.0)
    oEdges = edges;
}
