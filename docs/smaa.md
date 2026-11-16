# SMAA (Subpixel Morphological Anti-Aliasing)

> Menu: **Anti-Aliasing & Upscaling → Anti-Aliasing = SMAA, SMAA Quality** · Setting id(s): `aa.method`, `aa.smaa.quality` · Owner module: `aa.post`
> Values: Low / Medium / High / Ultra · Presets: Low=Low … Ultra=Ultra

Setting shown in the menu: `Anti-Aliasing: SMAA 1x` (+ `SMAA T2x` = SMAA + TAA-style temporal resolve)

## Definition

SMAA (Jimenez et al., 2012) is a morphological post-process AA: it detects edges, recognises the *shape* of the aliased staircase (L, Z and U patterns), and computes exactly how much each edge pixel should blend with its neighbour to reconstruct the original straight line. Sharper than FXAA (no texture blur) at slightly higher cost.

## How it works

Three full-screen passes:

1. **Edge detection** → `edgesTex` (RG8: R = edge on left, G = edge on top). Luma or colour difference with the left/top neighbour above a threshold (0.1), plus *local contrast adaptation*: an edge is dropped if a neighbouring edge is more than 2x stronger (prevents double edges).
2. **Blending weight calculation** → `blendTex` (RGBA8). For each edge pixel:
   - search left/right (or up/down) along the edge for its ends, using bilinear fetches on `edgesTex` to read 2 pixels per tap;
   - read the *crossing edges* at both ends to classify the pattern (L, Z, U);
   - look up a precomputed **area texture** (160x560 RG8) indexed by (pattern, distance to each end) that stores the exact coverage area of the ideal line through that pixel;
   - a small **search texture** speeds up the end-search.
   - optional diagonal and corner detection.
3. **Neighbourhood blending**: each pixel blends with its 4 neighbours by the weights from pass 2 (again using bilinear taps so one fetch blends two pixels).

## Breakdown

| Pass | Output | Cost at 1080p |
|---|---|---|
| Edge detection | RG8 + stencil mask | ~0.1 ms |
| Blend weights | RGBA8 (only on edge pixels via stencil) | ~0.3 ms |
| Neighbourhood blend | final colour | ~0.1 ms |

Presets: Low (no diagonals, 4 search steps), Medium (8), High (16, diagonals), Ultra (32, diagonals + corners).

**SMAA T2x** adds sub-pixel jitter (2 positions) and blends with the reprojected previous frame using motion vectors, catching temporal shimmer. **SMAA S2x / 4x** combine with MSAA 2x.

Code: `src/modules/aa/post_aa.cpp`, `shaders/aa/smaa_edge.frag`, `smaa_weights.frag`, `smaa_blend.frag`. This prototype uses the analytic coverage formula below instead of the reference area/search textures, so no binary lookup tables ship; diagonal and corner detection are omitted.

## Optimization

- Use the stencil buffer: pass 1 writes stencil where edges exist, passes 2-3 only run there. Most pixels are skipped.
- Luma edge detection is cheaper than colour; depth-based edge detection is cheapest but misses shading edges.
- Run in gamma space (sRGB) for perceptually even thresholds.
- Use predication (depth or normals) to lower the threshold where geometry edges are known.

### Measured cost

Measured cost at 1920×1080 (`gfxlab --bench-settings`), GPU: **Mesa llvmpipe 25.2.8 (software rasterizer, CPU)**. Deltas are against the High preset (TAA, CAS, AF 16x, native). On llvmpipe a whole frame takes hundreds of ms and the run-to-run noise is about ±8 ms (changing a setting with no rendering effect moves it that much), so only large differences mean anything; on a real GPU every pass here costs well under 1 ms. Greyed-out vendor options (DLSS, FSR 3, XeSS) snap back to our version, so they aren't listed.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Anti-Aliasing | Off | -22.2 | -21.9 | +0.0 | +0.2 |
| Anti-Aliasing | FXAA | +1.0 | -1.1 | +0.0 | +0.2 |
| Anti-Aliasing | SMAA | +6.0 | +6.0 | +0.0 | +0.3 |
| Anti-Aliasing | TAA | +0.9 | +0.7 | +0.0 | +0.3 |
| SMAA Quality | Low | -2.0 | -2.5 | +0.0 | +0.0 |
| SMAA Quality | Medium | -6.2 | -6.9 | +0.0 | +0.0 |
| SMAA Quality | High | -2.6 | -3.6 | +0.0 | +0.0 |
| SMAA Quality | Ultra | -4.1 | -3.9 | +0.0 | +0.0 |

### Measured quality

Measured with `tests/aa/quality_test.py` (512x288 procedural scene: thin lines, a moving rotated square, HDR highlights; Mesa llvmpipe). PSNR against a 16x16 supersampled reference, higher is better.

| Method | PSNR |
|---|---|
| No AA | 32.9 dB |
| FXAA High | 37.4 dB |
| SMAA High (ours, analytic) | 40.1 dB |

SMAA touched 6.2% of pixels as edges; everything else exits after pass 1.

## Rebuild (from scratch)

The simplified rebuild in this prototype keeps the three-pass structure but replaces the area-texture lookup with an analytic coverage formula for L-shaped edges (the most common pattern):

```
// pixel at distance d from the left end of a run of length n, edge at a crossing:
// the ideal line goes from (0, 0.5) to (n, 0) [L shape], coverage of pixel i is the trapezoid area
area(i) = 0.5 * (1 - (i + 0.5) / n)     // for the half of the span nearer the step
```

Steps:
1. Edge pass: `edges = step(threshold, abs(L - L_left), abs(L - L_top))`.
2. For each pixel with an edge, march left and right until the edge stops (max 16 steps), read crossing edges at each end, classify L/Z/U.
3. Compute coverage from the analytic formula (`lineArea()` in `smaa_weights.frag`). The reference implementation looks the same value up in the area texture, which also encodes diagonal patterns and rounded corners.
4. Blend pass: `color = mix(c, c_neighbor, weight)` per direction.
5. Debug views: show `edgesTex` and `blendTex` to see what each stage does.
