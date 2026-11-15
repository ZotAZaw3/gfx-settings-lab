#version 450
// FXAA 3.11-style quality pass (rewritten from the published algorithm). Input: tonemapped LDR.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uTex;             // linear clamp
layout(std140, binding = 3) uniform FxaaParams {
    vec4 uRcpSize;          // xy = 1/size
    float uEdgeThreshold;   // 0.125 high .. 0.25 low
    float uEdgeThresholdMin;// 0.0312
    float uSubpix;          // 0.75
    int   uSearchSteps;     // 4 / 8 / 12
    float uRange;           // 1 SDR, peak/paperwhite HDR
    float _p0, _p1, _p2;
};
// input is display-referred LINEAR (engine ToneMap output), so take sqrt for a perceptual luma
float L(vec2 uv) { return sqrt(max(dot(textureLod(uTex, uv, 0.0).rgb / uRange, vec3(0.299, 0.587, 0.114)), 0.0)); }
const float kSteps[12] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0);
void main() {
    vec2 px = uRcpSize.xy, uv = vUV;
    float m = L(uv);
    float n = L(uv + vec2(0, -px.y)), s = L(uv + vec2(0, px.y));
    float e = L(uv + vec2(px.x, 0)),  w = L(uv + vec2(-px.x, 0));
    float lo = min(m, min(min(n, s), min(e, w)));
    float hi = max(m, max(max(n, s), max(e, w)));
    float range = hi - lo;
    if (range < max(uEdgeThresholdMin, hi * uEdgeThreshold)) { oColor = textureLod(uTex, uv, 0.0); return; }
    float nw = L(uv + vec2(-px.x, -px.y)), ne = L(uv + vec2(px.x, -px.y));
    float sw = L(uv + vec2(-px.x, px.y)),  se = L(uv + vec2(px.x, px.y));
    // subpixel blend factor
    float avg = (2.0 * (n + s + e + w) + nw + ne + sw + se) / 12.0;
    float sub = clamp(abs(avg - m) / range, 0.0, 1.0);
    sub = smoothstep(0.0, 1.0, sub); sub = sub * sub * uSubpix;
    // edge orientation
    float eh = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);
    float ev = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);
    bool horiz = eh >= ev;
    float l1 = horiz ? n : w, l2 = horiz ? s : e;        // negative side, positive side
    float g1 = abs(l1 - m), g2 = abs(l2 - m);
    bool neg = g1 >= g2;
    float stepLen = horiz ? px.y : px.x;
    float lEdge; float grad;
    if (neg) { stepLen = -stepLen; lEdge = 0.5 * (l1 + m); grad = g1; }
    else     { lEdge = 0.5 * (l2 + m); grad = g2; }
    vec2 cur = uv;
    if (horiz) cur.y += stepLen * 0.5; else cur.x += stepLen * 0.5;
    vec2 off = horiz ? vec2(px.x, 0) : vec2(0, px.y);
    float gScaled = 0.25 * grad;
    vec2 p1 = cur - off, p2 = cur + off;
    float e1 = L(p1) - lEdge, e2 = L(p2) - lEdge;
    bool d1 = abs(e1) >= gScaled, d2 = abs(e2) >= gScaled;
    for (int i = 1; i < uSearchSteps && !(d1 && d2); i++) {
        if (!d1) { p1 -= off * kSteps[i]; e1 = L(p1) - lEdge; d1 = abs(e1) >= gScaled; }
        if (!d2) { p2 += off * kSteps[i]; e2 = L(p2) - lEdge; d2 = abs(e2) >= gScaled; }
    }
    float dist1 = horiz ? uv.x - p1.x : uv.y - p1.y;
    float dist2 = horiz ? p2.x - uv.x : p2.y - uv.y;
    bool closer1 = dist1 < dist2;
    float dist = min(dist1, dist2), span = dist1 + dist2;
    float pixOff = 0.5 - dist / span;
    bool mLess = (m - lEdge) < 0.0;
    bool correct = ((closer1 ? e1 : e2) < 0.0) != mLess;   // edge end must be on the right side
    float finalOff = max(correct ? pixOff : 0.0, sub);
    vec2 fuv = uv;
    if (horiz) fuv.y += finalOff * stepLen; else fuv.x += finalOff * stepLen;
    oColor = vec4(textureLod(uTex, fuv, 0.0).rgb, 1.0);
}
