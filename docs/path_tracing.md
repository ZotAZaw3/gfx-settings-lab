# Path Tracing

Settings (Ray Tracing tab): `Path Tracing` (`rt.pathtracing`, replaces the raster lighting
entirely), `Samples per Pixel per Frame` (1-8), `Max Bounces` (1-8), `Russian Roulette`,
`Accumulate When Still`, `Firefly Clamp`. Shared: `RT Resolution Scale`, `Sun Angular Radius`,
`RT Denoiser` (a-trous only, while fewer than 64 frames are accumulated).
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md).

## Definition

Path tracing renders the whole image by simulating light transport: for each
pixel, follow a random path from the camera, bouncing off surfaces, until it
reaches a light or escapes to the sky. Average many such paths and you get the
exact solution of the **rendering equation**, including shadows,
reflections, refraction, GI, AO, soft shadows and caustics in one algorithm.

The difference from "RT effects": hybrid RT adds ray-traced shadows /
reflections / GI on top of a rasterized image, one effect at a time. Path
tracing replaces the lighting completely ("full RT", as in Cyberpunk's RT
Overdrive or Alan Wake 2). It's what offline renderers (Cycles, Arnold,
RenderMan) do, run at 1-2 samples per pixel and then denoised.

## How it works

The rendering equation:

```
Lo(x, ωo) = Le(x, ωo) + ∫ f(x, ωi, ωo) · Li(x, ωi) · (n·ωi) dωi
```

Monte Carlo estimate, one path per sample:

```
radiance = 0; throughput = 1
ray = camera ray through pixel (jittered for AA)
for bounce in 0..maxBounces:
    if !traceClosest(ray, hit): radiance += throughput * sky(ray.dir); break
    radiance += throughput * emissive(hit)
    // Next-event estimation: sample the sun directly with a shadow ray
    radiance += throughput * brdf(hit, L) * sun * visibility(hit, L) * dot(N, L)
    // Choose the next direction by importance sampling the BRDF
    (dir, pdf, weight) = sampleBRDF(hit, -ray.dir)   // diffuse: cosine, specular: GGX VNDF
    throughput *= weight                              // = brdf*cos/pdf
    // Russian roulette after 2 bounces
    if bounce > 2: p = max(throughput); if rand() > p break; throughput /= p
    ray = (hit.pos + N*bias, dir)
```

Key ideas:

- **Importance sampling**: shoot rays where the BRDF is big (cosine lobe for
  diffuse, GGX lobe for specular) so each sample counts more.
- **Next-event estimation (NEE)**: at every bounce, explicitly connect to a
  light with a shadow ray instead of hoping a random bounce hits the small
  sun. Massive noise reduction for small/bright lights.
- **Multiple importance sampling (MIS)**: combine BRDF sampling and light
  sampling with weights so neither causes fireflies. (This prototype uses NEE
  for the sun and BRDF sampling for everything else; the sun is a delta-ish
  light with no BRDF-hit path, so no MIS weighting is needed.)
- **Russian roulette**: randomly kill low-energy paths, dividing survivors by
  the survival probability. Unbiased, and it saves most deep-bounce cost.
- **Progressive accumulation**: when the camera is still, average frames
  `accum = lerp(accum, new, 1/(n+1))`. Converges to a noise-free reference.
  Any camera or scene change resets `n`.

## Breakdown

| Step | Cost (1080p, compute BVH, 1 spp, 3 bounces) |
|---|---|
| Primary ray (or reuse G-buffer as bounce 0) | ~1-2 ms (0 if G-buffer reuse) |
| Each extra bounce (closest hit + NEE shadow ray) | ~2-4 ms |
| Accumulation | ~0.1 ms |
| Denoise (SVGF / OIDN / NRD / Ray Reconstruction) | 1-3 ms |

Hardware RT on a modern GPU: ~4-12 ms for 1-2 spp at 1080p with 2-3 bounces,
then denoise + upscale + frame generation make it playable. On the compute
fallback, the app defaults to half resolution and accumulation.

## Optimization

- **Reuse the G-buffer for bounce 0**: the primary hit is exactly what the
  rasterizer already computed; start paths at the G-buffer surface.
- **Russian roulette + low max bounces** (2-3 for diffuse scenes).
- **ReSTIR DI / ReSTIR GI / ReSTIR PT**: resample light and path candidates
  across neighbouring pixels and frames; turns 1 spp into many effective spp.
  This is what makes real-time path tracing viable in shipped games.
- **Radiance cache (SHaRC / NRC)**: terminate paths early into a world-space
  cache of learned/accumulated radiance.
- **Denoise + upscale**: trace at lower res, use a denoiser, then DLSS/FSR, or
  use **ray reconstruction** to do both in one network (see
  [denoising.md](denoising.md)).
- **Firefly clamp**: clamp per-sample radiance after the first bounce
  (biased but kills isolated very bright pixels).
- **Shader execution reordering (SER)** on RTX 40+: reorder threads by hit
  material so divergent shading runs coherently.
- **Opaque-only geometry, simple materials** on secondary bounces.

## In this app

Code: `shaders/raytracing/rt_pathtrace.comp` (sets `ctx.replaceLighting`) → optional a-trous → `rt_composite.frag` (×albedo) → `scene.color`.
Shared code: `src/modules/raytracing/bvh.cpp` (BVH), `shaders/raytracing/rt_common.glsl` (traversal, sampling),
`rt_gbuffer.glsl` (G-buffer reads).

Measured cost (Δ GPU ms / Δ CPU ms / Δ VRAM / Δ RAM per value at 1920×1080) is filled in from
`gfxlab --bench-settings` once the app builds; cloud numbers come from Mesa llvmpipe (software)
and only show relative cost.

## Rebuild (from scratch)

1. BVH + `traceClosest` + `traceAny` with barycentrics.
2. RGBA32F accumulation texture + frame counter (reset on camera move, setting
   change, or resize).
3. Compute pass per pixel: build a jittered camera ray (or start from the
   G-buffer); loop bounces as in the pseudocode with cosine diffuse sampling,
   GGX VNDF specular sampling chosen by Fresnel, NEE to the sun, Russian
   roulette.
4. Write `accum = (accum * n + sample) / (n + 1)`.
5. Optional denoise (when moving) → tonemapping/post as usual. With path
   tracing on, the raster lighting, SSAO, SSR and shadow maps are all skipped.
