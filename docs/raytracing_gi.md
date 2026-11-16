# Ray-Traced Global Illumination (RTGI)

Settings (Ray Tracing tab): `RT Global Illumination` (`rt.gi`), `GI Rays per Pixel` (1-2),
`GI Bounces` (1-3; 2+ reads last frame's GI at hit points). Shared: `RT Resolution Scale`,
`RT Denoiser`.
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md).

## Definition

**Direct light** goes light → surface → eye. **Global (indirect) illumination**
is light that bounced off other surfaces first: a red wall tinting the white
floor next to it, sunlight entering a window and lighting the whole room,
dark interiors that are actually dark. Without GI, games fake it with a flat
ambient term, baked lightmaps or light probes. RTGI computes **diffuse
indirect light** at runtime, so it works with moving lights and objects.

## How it works

1. From the G-buffer: `P`, `N`, albedo.
2. Generate a **cosine-weighted** hemisphere direction around `N`.
3. Trace a **closest-hit** ray (with a max distance, e.g. 50 m).
4. At the hit point, compute how much light *leaves* it toward `P`:
   - direct sun on the hit point: `albedo_hit * sunColor * max(dot(N_hit, L),0)`
     multiplied by an **RT shadow ray** from the hit point,
   - plus the hit's emissive,
   - plus (multi-bounce) last frame's GI at the hit point, read from the
     previous-frame GI texture if the hit is on screen, or a constant ambient.
5. On miss: sky radiance.
6. Because the direction was cosine-sampled, the Monte Carlo estimate of
   irradiance is just the radiance returned (π and the cosine cancel).
7. Denoise heavily (GI is very noisy at 1 spp), then
   `color += albedo * giIrradiance`. Ambient occlusion is implied.

### Multi-bounce for free

Feeding last frame's result into the hit shading is an **infinite-bounce
approximation**: each frame adds one bounce. Lumen-style and many RTGI
implementations do this (or use a radiance cache / probes).

## Breakdown

| Step | Cost (1080p, compute BVH, half res, 1 rpp) |
|---|---|
| Trace closest hit | ~1.0-2.0 ms |
| Shadow ray from hit | ~0.3-0.6 ms |
| Temporal accumulation | ~0.1 ms |
| Spatial filter (3-4 à-trous passes) | ~0.4 ms |
| Upsample + composite | ~0.1 ms |

## Optimization

- **Half or quarter resolution.** Diffuse GI is the lowest frequency signal
  in the frame.
- **Radiance caching / probes** (DDGI, ReSTIR GI, SHaRC, Lumen's surface
  cache): trace from a sparse set of probes or cache radiance in world space
  and interpolate. Turns per-pixel rays into per-probe rays.
- **ReSTIR GI**: reuse good samples spatially and temporally so 1 ray/pixel
  behaves like ~10+.
- **Shorter rays + screen-space first**: try a short screen-space trace and
  only use the BVH when it fails.
- **Cheap hit shading**: no normal maps, low mip, shadow map instead of a ray
  for the hit's sun visibility.
- **Turn RTAO off** when RTGI is on (it already includes occlusion).
- **Temporal stability**: clamp history to the neighbourhood (variance clip)
  to limit ghosting when lights move.

## In this app

Code: `shaders/raytracing/rt_gi.comp` → denoiser → `lighting.indirectDiffuse` (irradiance/π, so diffuse = albedo × value).
Shared code: `src/modules/raytracing/bvh.cpp` (BVH), `shaders/raytracing/rt_common.glsl` (traversal, sampling),
`rt_gbuffer.glsl` (G-buffer reads).

### Measured cost

Measured with `gfxlab --bench-settings` on **Mesa llvmpipe 25.2 (software rasterizer, LLVM 20, 256-bit)**, 640×360, procedural demo scene, High preset baseline (RT AO and RT reflections on, RT resolution 0.75). Each value is the 60-frame average after the change minus the one before it. On llvmpipe the "GPU" is the CPU, so GPU and CPU ms move together, and run-to-run noise is about ±3 ms. Treat these as **relative** costs; a real GPU is 10-100x faster.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB |
|---|---|---|---|---|
| `rt.gi` | Off | -0.4 | +0.1 | +0.0 |
| `rt.gi` | On | +3.2 | +3.2 | +4.6 |
| `rt.gi.rays` | 1 | -1.1 | -0.7 | +0.0 |
| `rt.gi.rays` | 2 | -0.2 | +0.5 | +0.0 |
| `rt.gi.bounces` | 1 | +1.3 | +1.8 | +0.0 |
| `rt.gi.bounces` | 2 | -1.0 | -1.4 | +0.0 |
| `rt.gi.bounces` | 3 | +1.0 | +1.6 | +0.0 |

Turning GI on shows only +3.2 ms because it also switches RT AO off (−17.5 ms), so RT GI itself costs about 20 ms here, a little more than the AO it replaces. Extra bounces are nearly free because they read last frame's GI instead of tracing more rays.

## Rebuild (from scratch)

1. BVH + `traceClosest` + `traceAny`.
2. RGBA16F GI texture + history texture.
3. Compute pass: reconstruct `P/N`; cosine sample; `traceClosest`; on hit
   interpolate normal/material, compute sun term × `traceAny` shadow ray,
   add emissive, add `prevGI(hitPos) * albedo_hit`; on miss sample sky.
4. Temporal accumulate (alpha ≈ 0.05-0.1) with disocclusion reset.
5. 3-4 iterations of à-trous with luminance-variance guidance (SVGF).
6. Lighting: replace the flat ambient term with `albedo * gi`.
