# RT Quality Knobs: Rays per Pixel, Bounces, RT Resolution, Presets

Settings (Ray Tracing tab): `RT Quality Preset` (Custom / Low / Medium / High / Ultra /
Psycho), `RT Resolution Scale` (0.25-1), `Max Ray Distance (m)`, plus the per-effect rays,
bounces and roughness cutoff it drives.
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md).

## Definition

All RT effects are a Monte Carlo estimate. Three numbers decide quality vs.
cost:

- **Rays per pixel (rpp / spp)**: how many random samples each pixel takes per
  frame. Noise falls as `1 / sqrt(rpp)`: 4x the rays for half the noise.
- **Bounces**: how many times a path may scatter. Bounce 1 = direct
  reflections / one-bounce GI; 2-3 = light bouncing around a room; more
  matters only for very bright, enclosed scenes.
- **RT resolution**: tracing at 50% scale (a quarter of the pixels) then
  upsampling. Cost scales almost linearly with pixel count.

Game presets mostly just move these numbers (Cyberpunk's "Psycho" RT lighting
= more GI rays/bounces; "Ultra" reflections = full-res + lower roughness
cutoff).

## How it works

| Knob | Effect on image | Effect on cost |
|---|---|---|
| rpp 1 → 2 | noise × 0.71, denoiser blurs less | ~2x trace |
| bounces 1 → 2 | brighter, more coloured indirect light | +1 closest-hit + shadow per path that survives |
| RT res 100% → 50% | softer detail, denoiser handles upscale | ~0.25-0.3x trace |
| Max distance | far objects stop contributing | shorter traversals |
| Roughness cutoff (refl.) | rough surfaces use fallback | fewer rays |

Presets in this app:

| Preset | RT res | Shadows rpp | AO rpp | Refl. rpp / cutoff | GI rpp / bounces | PT spp / bounces |
|---|---|---|---|---|---|---|
| Low | 50% | 1 | 1 | 1 / 0.3 | 1 / 1 | 1 / 2 |
| Medium | 50% | 1 | 1 | 1 / 0.5 | 1 / 1 | 1 / 3 |
| High | 75% | 1 | 2 | 1 / 0.7 | 1 / 2 | 1 / 4 |
| Ultra | 100% | 2 | 2 | 1 / 1.0 | 2 / 2 | 2 / 5 |
| Psycho | 100% | 4 | 4 | 2 / 1.0 | 2 / 3 | 4 / 8 |

## Breakdown

Frame cost ≈ `pixels × scale² × Σ(effect rpp × avg ray cost × (1 + survivingBounces))`.
Average ray cost depends on BVH quality, ray length and coherence: primary-like
coherent rays are cheapest; diffuse GI rays are the most incoherent.

## Optimization

- Prefer **lower RT resolution + good denoiser/upscaler** over lower rpp.
- **Russian roulette** makes extra max bounces nearly free (most paths die).
- **Adaptive sampling**: more rays only where variance is high (penumbras,
  glossy reflections), fewer in converged regions.
- **Ray length limits** per effect (AO: 1 m, GI: 50 m, shadows: light distance).

### Measured cost

Measured with `gfxlab --bench-settings` on **Mesa llvmpipe 25.2 (software rasterizer, LLVM 20, 256-bit)**, 640×360, procedural demo scene, High preset baseline (RT AO and RT reflections on, RT resolution 0.75). Each value is the 60-frame average after the change minus the one before it. On llvmpipe the "GPU" is the CPU, so GPU and CPU ms move together, and run-to-run noise is about ±3 ms. Treat these as **relative** costs; a real GPU is 10-100x faster.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB |
|---|---|---|---|---|
| `rt.quality` | Custom | +0.6 | +1.0 | +0.0 |
| `rt.quality` | Low | -9.8 | -9.6 | +0.0 |
| `rt.quality` | Medium | -9.6 | -9.5 | +0.0 |
| `rt.quality` | High | -0.4 | -0.8 | +0.0 |
| `rt.quality` | Ultra | +7.2 | +6.6 | +0.0 |
| `rt.quality` | Psycho | +10.5 | +10.3 | +0.0 |
| `rt.resolution` | 0.25 | -25.0 | -25.0 | -11.6 |
| `rt.resolution` | 0.50 | -18.3 | -18.3 | -7.3 |
| `rt.resolution` | 1.00 | +4.5 | +4.6 | +10.2 |
| `rt.max_distance` | 10 | -1.3 | -1.0 | +0.0 |
| `rt.max_distance` | 200 | -1.9 | -2.0 | +0.0 |
| `rt.max_distance` | 1000 | -1.3 | -0.8 | +0.0 |
| `rt.bvh.dynamic` | Off | -0.3 | +0.8 | +0.0 |
| `rt.bvh.dynamic` | On | -0.7 | -0.1 | +0.0 |

RT resolution dominates cost, as the doc predicts: 0.25 saves ~25 ms and 0.5 saves ~18 ms against the 0.75 baseline, and 1.0 adds ~4.5 ms and 10 MB. The quality presets Low/Medium save ~9.7 ms and Psycho adds ~10.5 ms.

## Rebuild (from scratch)

Give every RT pass `rpp`, `maxBounces`, `maxDistance` uniforms and render its
output into a texture sized `renderRes * rtScale`, then upsample with a
depth-aware bilateral filter. Presets are just tables that write those
values.
