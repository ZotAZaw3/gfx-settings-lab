# Ray-Traced Ambient Occlusion (RTAO)

Settings (Ray Tracing tab): `RT Ambient Occlusion` (`rt.ao`), `AO Rays per Pixel` (1-8),
`AO Radius (m)` (0.1-5). Greyed out while RT GI is on. Shared: `RT Resolution Scale`,
`RT Denoiser`.
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md).
Compare with SSAO/HBAO+/GTAO in the shadows-AO thread's docs.

## Definition

Ambient occlusion darkens creases, corners and contact areas where ambient
light from the sky/environment can't easily reach. It's the fraction of the
hemisphere above a point that is **not** blocked within a short radius.

Screen-space AO (SSAO, GTAO) estimates this from the depth buffer only, so it
misses occluders that are off-screen or hidden behind the front layer, and it
produces halos around silhouettes. RTAO asks the real scene, so it is
view-independent and halo-free.

## How it works

1. From the G-buffer: `P`, `N`.
2. For each of `n` rays: generate a **cosine-weighted** direction on the
   hemisphere around `N` (more rays near the normal, matching how much each
   direction contributes to diffuse lighting).
3. Trace an **occlusion ray** with `tMax = radius`.
4. `AO = 1 - hits / n`. Optionally weight hits by distance
   (`1 - t/radius`) for a softer falloff.
5. Denoise, then the lighting pass multiplies **only the ambient/indirect**
   term by AO (never the direct sun light – that is the shadow's job).

```glsl
vec3 d = cosineSampleHemisphere(N, rand2(pixel, frame, i));
occl += traceAny(P + N*bias, d, aoRadius) ? 1.0 : 0.0;
```

## Breakdown

| Step | Cost (1080p, compute BVH, 1 rpp, 1 m radius) |
|---|---|
| Trace | ~0.4-0.9 ms (short rays touch few nodes) |
| Temporal accumulation | ~0.1 ms |
| Spatial filter | ~0.2 ms |

RTAO is the cheapest RT effect: occlusion-only rays with a tiny `tMax`.

## Optimization

- **Radius is the main cost knob.** 0.5-1.5 m looks right for indoor scenes;
  larger radii start to look like a bad GI.
- **1 ray per pixel + temporal accumulation** converges in ~8-16 frames.
- **Half resolution** is nearly invisible after denoising since AO is low
  frequency.
- **Skip the sky and far pixels** (AO fades out with distance anyway).
- **Cull by instance mask**: don't let particles or tiny alpha-tested foliage
  occlude AO rays.
- **Replace with RT GI** when GI is on: one-bounce GI with a short ray
  already contains the occlusion, so the app disables RTAO automatically when
  RT GI is enabled (it would double-darken).

## In this app

Code: `shaders/raytracing/rt_ao.comp` → denoiser → `lighting.ao` (multiplies indirect light only).
Shared code: `src/modules/raytracing/bvh.cpp` (BVH), `shaders/raytracing/rt_common.glsl` (traversal, sampling),
`rt_gbuffer.glsl` (G-buffer reads).

### Measured cost

Measured with `gfxlab --bench-settings` on **Mesa llvmpipe 25.2 (software rasterizer, LLVM 20, 256-bit)**, 640×360, procedural demo scene, High preset baseline (RT AO and RT reflections on, RT resolution 0.75). Each value is the 60-frame average after the change minus the one before it. On llvmpipe the "GPU" is the CPU, so GPU and CPU ms move together, and run-to-run noise is about ±3 ms. Treat these as **relative** costs; a real GPU is 10-100x faster.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB |
|---|---|---|---|---|
| `rt.ao` | Off | -17.5 | -17.5 | -11.2 |
| `rt.ao` | On | +1.1 | +1.6 | +0.0 |
| `rt.ao.rays` | 1 | -1.1 | -0.6 | +0.0 |
| `rt.ao.rays` | 2 | +0.9 | +1.4 | +0.0 |
| `rt.ao.rays` | 3 | -1.5 | -1.4 | +0.0 |
| `rt.ao.rays` | 4 | +0.8 | +0.7 | +0.0 |
| `rt.ao.rays` | 5 | +2.4 | +2.7 | +0.0 |
| `rt.ao.rays` | 6 | +0.9 | +1.6 | +0.0 |
| `rt.ao.rays` | 7 | -0.3 | -0.5 | +0.0 |
| `rt.ao.rays` | 8 | -0.9 | -0.9 | +0.0 |
| `rt.ao.radius` | 0.10 | -0.6 | +0.1 | +0.0 |
| `rt.ao.radius` | 1.00 | -3.5 | -3.7 | +0.0 |
| `rt.ao.radius` | 5.00 | -3.8 | -3.9 | +0.0 |

RT AO is part of the baseline, so its cost shows up as the saving when it is turned off: ~17.5 ms and 11.2 MB. Most of that is the SVGF denoiser (see denoising.md), not the short AO rays.

## Rebuild (from scratch)

1. BVH + `traceAny`.
2. `R8` AO texture.
3. Compute pass: reconstruct `P/N`; for i in rays: cosine hemisphere sample
   from a per-pixel/per-frame random; `traceAny(P+N*bias, d, radius)`;
   write `1 - hits/n`.
4. Temporal accumulate with depth-based disocclusion rejection.
5. Depth/normal-aware à-trous blur (2-3 iterations).
6. Lighting: `ambient *= ao`.
