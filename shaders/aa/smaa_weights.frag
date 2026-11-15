#version 450
// SMAA pass 2 (simplified, analytic): for every edge pixel find the run length along the edge,
// classify the ends by their crossing edges, and compute the coverage area of the ideal line
// analytically instead of using the precomputed area texture. Output RGBA8:
//   x = weight to blend with the pixel ABOVE (top edge), y = with the pixel BELOW
//   z = with the pixel to the LEFT (left edge),          w = with the pixel to the RIGHT
// Pass 3 reads these from this pixel and its neighbours.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oWeights;
layout(binding = 0) uniform sampler2D uEdges;
layout(std140, binding = 3) uniform SmaaParams { vec4 uRcpSize; /* xy 1/size, z range (1 SDR) */ float uThreshold; int uMaxSearch; int uDiag; float uContrastFactor; };
vec2 E(ivec2 p) { ivec2 lim = textureSize(uEdges, 0) - 1; if (any(lessThan(p, ivec2(0))) || any(greaterThan(p, lim))) return vec2(0); return texelFetch(uEdges, p, 0).rg; }

// Coverage for pixel i (0-based from the run start) of a run of n edge pixels. e1 / e2 describe the
// staircase step at the start / end of the run: +1 = the colour on the OTHER side of the edge extends
// into our row/column there (step towards us), -1 = our colour extends into the other side, 0 = no step.
// The ideal line joins the mid-points of the steps (MLAA): height +-0.5 at a stepped end, 0 at a flat
// end. L = one step, Z = opposite steps (straight line from +0.5 to -0.5), U = equal steps (two half
// lines meeting at 0 in the middle). The returned value is the mean height of the line inside pixel i:
// > 0 means that fraction of our pixel is really covered by the other side's colour.
float lineArea(float i, float n, float e1, float e2) {
    float x0 = i, x1 = i + 1.0, h0, h1;
    if (e1 != 0.0 && e1 == e2) {                       // U
        float mid = n * 0.5;
        if (x1 <= mid)      { h0 = 0.5 * e1 * (1.0 - x0 / mid); h1 = 0.5 * e1 * (1.0 - x1 / mid); }
        else if (x0 >= mid) { h0 = 0.5 * e2 * ((x0 - mid) / mid); h1 = 0.5 * e2 * ((x1 - mid) / mid); }
        else return 0.0;
    } else {                                           // L or Z
        h0 = mix(0.5 * e1, 0.5 * e2, x0 / n);
        h1 = mix(0.5 * e1, 0.5 * e2, x1 / n);
    }
    // the line may cross zero inside the pixel: only the part on each side counts (keep the dominant one)
    return (h0 * h1 >= 0.0) ? 0.5 * (h0 + h1) : 0.5 * (h0 * abs(h0) + h1 * abs(h1)) / (abs(h0) + abs(h1));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 e = E(p);
    vec4 w = vec4(0);
    if (e.g > 0.0) {   // edge on top: horizontal run, searched left/right along row p and row p-1 boundary
        int l = 0, r = 0;
        for (; l < uMaxSearch; l++) if (E(p + ivec2(-l - 1, 0)).g == 0.0) break;
        for (; r < uMaxSearch; r++) if (E(p + ivec2(r + 1, 0)).g == 0.0) break;
        float n = float(l + r + 1), i = float(l);
        // crossing edges at the ends (left edges just outside/at the run ends, above or below)
        float cl = E(p + ivec2(-l, 0)).r > 0.0 ? 1.0 : (E(p + ivec2(-l, -1)).r > 0.0 ? -1.0 : 0.0);
        float cr = E(p + ivec2(r + 1, 0)).r > 0.0 ? 1.0 : (E(p + ivec2(r + 1, -1)).r > 0.0 ? -1.0 : 0.0);
        float a = lineArea(i, n, cl, cr);
        // positive area: this pixel takes colour from above; negative: the pixel above takes ours
        w.x = max(a, 0.0); w.y = max(-a, 0.0);
    }
    if (e.r > 0.0) {   // edge on left: vertical run
        int u = 0, d = 0;
        for (; u < uMaxSearch; u++) if (E(p + ivec2(0, -u - 1)).r == 0.0) break;
        for (; d < uMaxSearch; d++) if (E(p + ivec2(0, d + 1)).r == 0.0) break;
        float n = float(u + d + 1), i = float(u);
        float cu = E(p + ivec2(0, -u)).g > 0.0 ? 1.0 : (E(p + ivec2(-1, -u)).g > 0.0 ? -1.0 : 0.0);
        float cd = E(p + ivec2(0, d + 1)).g > 0.0 ? 1.0 : (E(p + ivec2(-1, d + 1)).g > 0.0 ? -1.0 : 0.0);
        float a = lineArea(i, n, cu, cd);
        w.z = max(a, 0.0); w.w = max(-a, 0.0);
    }
    oWeights = w;
}
