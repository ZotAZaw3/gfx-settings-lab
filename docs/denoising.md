# Ray Tracing Denoising (and how Ray Reconstruction replaces it)

Settings (Ray Tracing tab): `RT Denoiser` (`rt.denoiser`: Off / Temporal Only / SVGF),
`Temporal Alpha (min)` (0.01-0.5), `A-Trous Iterations` (1-5), `Luminance Edge Sigma`
(debug). `RT Debug View` (debug) shows each effect raw or denoised.
The Ray Reconstruction setting itself (DLSS RR / FSR Ray Regeneration) is
owned by the anti-aliasing & upscaling thread; this page explains what it
replaces.

## Definition

Real-time RT traces 1 ray (or less) per pixel per effect. One random sample of
a soft shadow, glossy reflection or bounce light is extremely noisy. A
**denoiser** reconstructs a clean image from those noisy samples by reusing
information from **neighbouring pixels (spatial)** and **previous frames
(temporal)**, guided by noise-free G-buffer data (depth, normals, albedo,
roughness, motion vectors) so it doesn't blur across edges.

## How it works

This app implements a simplified **SVGF** (Spatiotemporal Variance-Guided
Filtering, Schied et al. 2017), the basis of most hand-written RT denoisers.

1. **Demodulate**: divide out albedo before filtering (`irradiance = color /
   albedo`) so texture detail isn't blurred; multiply back after.
2. **Temporal accumulation**:
   - Reproject each pixel to last frame using the previous view-projection
     matrix (or motion vectors).
   - Reject history if the reprojected depth/normal differ too much
     (**disocclusion**) or it falls off screen.
   - `history = lerp(history, current, α)` with α ≈ 0.1 (or `1/(n+1)` with a
     per-pixel sample count `n` capped at ~32, so fresh pixels converge fast).
   - Accumulate the first and second moments of luminance → per-pixel
     **variance** estimate.
3. **Variance clamp** (optional, not in this prototype): clamp history to
   `mean ± k·σ` of the current 3x3 neighbourhood to fight ghosting when lighting
   changes. This app rejects history by depth only.
4. **À-trous wavelet filter**: N iterations of a 5x5 kernel with holes,
   step size 1, 2, 4, 8, 16. Each tap's weight is
   `w = w_depth · w_normal · w_luminance`:
   - `w_depth = exp(-|z - z_i| / (σz · |∇z · offset| + ε))`
   - `w_normal = max(0, dot(n, n_i))^128`
   - `w_lum = exp(-|l - l_i| / (σl · sqrt(variance) + ε))`
   High-variance (noisy) regions blur more; converged regions stay sharp.
5. **Remodulate** by albedo and composite.

Effect-specific variants: shadows/AO are single-channel (cheap); reflections
add roughness to the weights and use hit distance for the radius; GI uses the
most iterations.

Production denoisers: **NVIDIA NRD** (ReBLUR, ReLAX, SIGMA for shadows),
**AMD FidelityFX Denoiser**, **Intel OIDN** (offline / interactive,
neural), and Unreal's built-in denoisers.

## Production denoisers compared (NRD and friends)

| Denoiser | Vendor | Type | Signals | Notes |
|---|---|---|---|---|
| **SVGF** | paper (NVIDIA Research 2017) | temporal + à-trous | any | What this app implements; simple, a bit blurry |
| **A-SVGF** | paper 2018 | SVGF + gradient-based history rejection | any | Reacts faster to moving lights |
| **NRD ReBLUR** | NVIDIA | recurrent blur, hit-distance driven | diffuse + specular | Default in many RTX titles; best detail at low rpp |
| **NRD ReLAX** | NVIDIA | SVGF-derived, tuned for ReSTIR | diffuse + specular | Pairs with RTXDI/ReSTIR, sharper but needs more rays |
| **NRD SIGMA** | NVIDIA | specialised | shadows (and translucent shadows) | Penumbra-size-aware blur |
| **FidelityFX Denoiser** | AMD | temporal + spatial | shadows, reflections | Open source, GPU-agnostic |
| **OIDN** | Intel | neural (U-Net) | path tracing beauty + albedo/normal | Offline/interactive; GPU support in 2.x |
| **OptiX denoiser** | NVIDIA | neural | path tracing | Offline viewports (Blender, Omniverse) |
| **DLSS RR / FSR Ray Regeneration** | NVIDIA / AMD | neural, combined with upscaling | everything | See next section |

NRD integration in brief: you render the noisy diffuse and specular radiance
plus **hit distance** (packed with `REBLUR_FrontEnd_PackRadianceAndNormHitDist`),
supply `IN_MV`, `IN_NORMAL_ROUGHNESS`, `IN_VIEWZ`, call
`nrd::Integration::Denoise` per frame with the camera matrices, then
composite `OUT_DIFF_RADIANCE_HITDIST` / `OUT_SPEC_RADIANCE_HITDIST` with
albedo. It's API-agnostic (D3D11, D3D12, Vulkan via NRI), so it could slot in
to this app's D3D11 path; it's not included because it's an external SDK.

## Ray Reconstruction (what replaces the denoiser)

The classic chain is:

```
noisy RT  ->  denoiser per effect (hand-tuned)  ->  TAA / DLSS / FSR upscaler
```

Two separate temporal filters fight each other: the denoiser smears detail
the upscaler wanted, and each effect's denoiser is tuned separately.

**Ray Reconstruction** (NVIDIA DLSS 3.5+ RR; AMD FSR "Ray Regeneration" in
FSR Redstone) is a **single neural network that replaces all the per-effect
denoisers *and* the upscaler**. It takes:

- the noisy, combined RT signal (or separated diffuse/specular),
- guide buffers: depth, normals, roughness, diffuse albedo, specular albedo,
  motion vectors, specular hit distance,

and outputs a denoised, upscaled, anti-aliased frame in one pass. Because it
was trained on huge amounts of path-traced reference data it keeps detail
(reflections, shadow edges) that hand-written filters blur, and it recognises
patterns of noise vs. real detail.

In this app: when the AA/upscaling setting `aa.ray_reconstruction` is not Off,
the RT module **skips all of its temporal and a-trous passes**, composites the
raw signal, and exports `rt.noisy` (noisy lit image ÷ albedo) and `rt.albedo`
at render resolution. The AA thread's "RR-lite" mode feeds those into its
temporal upscaler (one joint denoise + upscale, like RR); "DLSS RR" is a stub
that needs NVIDIA's SDK. See [ray_reconstruction.md](ray_reconstruction.md).

## Breakdown

| Step | Cost (1080p) |
|---|---|
| Temporal accumulation + moments | ~0.1-0.2 ms per effect |
| Variance estimate (3x3) | ~0.05 ms |
| À-trous, per iteration | ~0.1-0.15 ms (RGBA16F) |
| Total for GI (4 iterations) | ~0.6-0.8 ms |
| DLSS RR (replaces all denoisers + upscaler) | ~1-2 ms on RTX 40 |

## Optimization

- **Denoise at trace resolution** (half res) and upsample afterwards.
- **Share one temporal pass** across effects by packing (shadow, AO) into an
  RG texture.
- **Fewer iterations for low-frequency signals** (AO: 2, shadows: 2, GI: 4).
- **Per-pixel sample count** instead of a fixed α: converged pixels use a
  long history; disoccluded pixels adapt in 2-3 frames.
- **Blue-noise sampling** makes 1 spp noise easier to filter (energy pushed
  to high frequencies the à-trous removes first).
- **Hit-distance-aware radius** for shadows and reflections (keeps contact
  details sharp).
- Use **Ray Reconstruction** when available: higher quality and it removes the
  per-effect denoiser cost entirely.

## In this app

Code: `shaders/raytracing/rt_denoise_temporal.comp`, `rt_denoise_atrous.comp`; driver code in `src/modules/raytracing/raytracing_module.cpp` (`denoise()`).
Shared code: `src/modules/raytracing/bvh.cpp` (BVH), `shaders/raytracing/rt_common.glsl` (traversal, sampling),
`rt_gbuffer.glsl` (G-buffer reads).

### Measured cost

Measured with `gfxlab --bench-settings` on **Mesa llvmpipe 25.2 (software rasterizer, LLVM 20, 256-bit)**, 640×360, procedural demo scene, High preset baseline (RT AO and RT reflections on, RT resolution 0.75). Each value is the 60-frame average after the change minus the one before it. On llvmpipe the "GPU" is the CPU, so GPU and CPU ms move together, and run-to-run noise is about ±3 ms. Treat these as **relative** costs; a real GPU is 10-100x faster.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB |
|---|---|---|---|---|
| `rt.denoiser` | Off | -28.2 | -28.3 | +0.0 |
| `rt.denoiser` | Temporal Only | -22.7 | -22.4 | +0.0 |
| `rt.denoiser` | SVGF | +1.1 | +2.0 | +0.0 |
| `rt.denoiser.temporal_alpha` | 0.01 | -0.4 | -0.4 | +0.0 |
| `rt.denoiser.temporal_alpha` | 0.05 | -1.3 | -0.8 | +0.0 |
| `rt.denoiser.temporal_alpha` | 0.50 | -0.4 | -0.2 | +0.0 |
| `rt.denoiser.atrous_iterations` | 1 | -19.3 | -18.6 | +0.0 |
| `rt.denoiser.atrous_iterations` | 2 | -13.2 | -12.8 | +0.0 |
| `rt.denoiser.atrous_iterations` | 3 | -8.9 | -8.5 | +0.0 |
| `rt.denoiser.atrous_iterations` | 4 | -0.9 | -0.3 | +0.0 |
| `rt.denoiser.atrous_iterations` | 5 | +3.5 | +4.4 | +0.0 |

With RT AO and reflections on, the SVGF denoiser costs ~28 ms in total, and temporal-only costs ~5 ms. The à-trous filter is the expensive part on this software renderer because each of its 25 taps re-reads four G-buffer textures. The obvious next optimization is to pack depth and normal into one trace-resolution texture, read once per tap.

## Rebuild (from scratch)

1. Textures per effect: current (noisy), history color, history moments
   (RG: mean lum, mean lum²), sample count.
2. Temporal compute pass: reproject with `prevViewProj * worldPos`, test
   depth/normal, `n = min(n+1, 32)`, `α = max(1/n, αmin)`, blend color and
   moments, write variance = `m2 - m1²`.
3. À-trous compute pass run N times ping-ponging two textures, step `1<<i`,
   weights as above. Feed the output of iteration 0 back as next frame's
   history (SVGF does this to stabilise).
4. Composite into the lighting.
5. If ray reconstruction is enabled in the upscaler, bypass steps 2-3 and pass
   the noisy signal plus guide buffers to the upscaler.
