# FXAA (Fast Approximate Anti-Aliasing)

> Menu: **Anti-Aliasing & Upscaling → Anti-Aliasing = FXAA, FXAA Quality** · Setting id(s): `aa.method`, `aa.fxaa.quality` · Owner module: `aa.post`
> Values: Low / Medium / High · Presets: Low=Low, Medium=Medium, High=High, Ultra=High

Setting shown in the menu: `Anti-Aliasing: FXAA` (+ `FXAA Quality: Low / Medium / High`)

## Definition

FXAA (Timothy Lottes, NVIDIA, 2009) is a single full-screen post-process pass that finds high-contrast edges in the final image and blurs along them. It knows nothing about geometry, so it smooths every kind of edge (geometry, alpha test, shader aliasing) at almost no cost, but it also softens texture detail and can't fix temporal shimmer.

## How it works

Run on the tone-mapped LDR image (luma must be perceptual):

1. **Luma:** compute luminance of the pixel and its 4 neighbours (N, S, E, W).
2. **Early exit:** if `max - min < max(edgeThresholdMin, max * edgeThreshold)` the pixel is not on an edge; output it unchanged. Most pixels exit here.
3. **Edge direction:** use the 3x3 neighbourhood to decide if the edge is horizontal or vertical (compare horizontal vs vertical second derivatives).
4. **Edge side:** pick which of the two perpendicular neighbours has the steeper gradient; step half a pixel toward it so we sit exactly on the edge.
5. **End-of-edge search:** walk along the edge in both directions (with growing step sizes: 1, 1, 1, 1.5, 2, 2, 4, 8 …) until the luma along the edge changes by more than the local gradient. This finds where the staircase step starts and ends.
6. **Blend amount:** the closer the pixel is to the end of the edge span, the more it should be blended. Offset = `0.5 - min(distNeg, distPos) / spanLength`.
7. **Subpixel aliasing:** additionally blend by how much the pixel differs from its 3x3 average (fixes single-pixel sparkles).
8. Re-sample the input with bilinear filtering at the offset UV. The hardware bilinear blend does the actual anti-aliasing.

## Breakdown

| Stage | Notes |
|---|---|
| Inputs | LDR colour (luma in alpha, or computed on the fly) |
| Cost | ~0.2-0.4 ms at 1080p on a mid-range GPU |
| Passes | 1 |
| Fixes | geometry edges, alpha test, some specular |
| Doesn't fix | temporal crawling, sub-pixel triangles that disappear |
| Side effect | texture blur (worse at higher subpix setting) |

Quality presets map to: search steps (Low 4, Medium 8, High 12), `edgeThreshold` (0.250 / 0.166 / 0.125), `subpix` (0.5 / 0.75 / 1.0).

Code: `src/modules/aa/post_aa.cpp`, `shaders/aa/fxaa.frag`.

## Optimization

- Store luma in the alpha channel during tone mapping, so FXAA reads 1 channel for edge detection.
- Use `textureGather` (GL 4.0 / D3D11 `GatherRed`) to read 4 luma values in one fetch.
- Lower `subpix` if textures look smeared; raise `edgeThresholdMin` to skip dark noise.
- Apply before UI and film grain, after tone mapping.
- Pair with CAS sharpening to win back texture detail.

### Measured quality

Measured with `tests/aa/quality_test.py` (512x288 procedural scene: thin lines, a moving rotated square, HDR highlights; Mesa llvmpipe). PSNR against a 16x16 supersampled reference, higher is better.

| Method | PSNR |
|---|---|
| No AA | 32.9 dB |
| FXAA High | 37.4 dB |
| SMAA High (ours) | 40.1 dB |

## Rebuild (from scratch)

A complete simplified FXAA is ~60 lines; the full one ships in `shaders/aa/fxaa.frag`. Core:

```glsl
float L(vec2 uv) { return dot(texture(uTex, uv).rgb, vec3(0.299, 0.587, 0.114)); }
void main() {
    vec2 px = 1.0 / vec2(textureSize(uTex, 0));
    float m = L(uv), n = L(uv + vec2(0, px.y)), s = L(uv - vec2(0, px.y));
    float e = L(uv + vec2(px.x, 0)), w = L(uv - vec2(px.x, 0));
    float lo = min(m, min(min(n, s), min(e, w))), hi = max(m, max(max(n, s), max(e, w)));
    if (hi - lo < max(0.0312, hi * 0.125)) { o = texture(uTex, uv); return; }
    bool horiz = abs(n + s - 2.0 * m) >= abs(e + w - 2.0 * m);
    float g1 = horiz ? s : w, g2 = horiz ? n : e;
    bool neg = abs(g1 - m) >= abs(g2 - m);
    float step = (horiz ? px.y : px.x) * (neg ? -1.0 : 1.0);
    vec2 edgeUV = uv + (horiz ? vec2(0, step * 0.5) : vec2(step * 0.5, 0));
    vec2 dir = horiz ? vec2(px.x, 0) : vec2(0, px.y);
    float edgeL = 0.5 * (m + (neg ? g1 : g2)), grad = 0.25 * max(abs(g1 - m), abs(g2 - m));
    vec2 p1 = edgeUV - dir, p2 = edgeUV + dir; bool d1 = false, d2 = false;
    for (int i = 0; i < 12 && !(d1 && d2); i++) {
        if (!d1) { d1 = abs(L(p1) - edgeL) >= grad; if (!d1) p1 -= dir; }
        if (!d2) { d2 = abs(L(p2) - edgeL) >= grad; if (!d2) p2 += dir; }
    }
    float dn = horiz ? uv.x - p1.x : uv.y - p1.y, dp = horiz ? p2.x - uv.x : p2.y - uv.y;
    float off = 0.5 - min(dn, dp) / (dn + dp);
    // (full version also checks that the edge end is on the correct side and adds subpixel blend)
    o = texture(uTex, uv + (horiz ? vec2(0, off * step) : vec2(off * step, 0)));
}
```

Steps:
1. Tone map to LDR (sRGB or gamma-space luma).
2. Full-screen triangle with this shader, linear-clamp sampler.
3. Expose the three quality knobs.
4. Compare screenshots with AA off on a scene with thin wires and a high-contrast horizon.
