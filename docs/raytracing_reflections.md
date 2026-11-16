# Ray-Traced Reflections

Settings (Ray Tracing tab): `RT Reflections` (`rt.reflections`), `Reflection Rays per Pixel`
(1-2), `Max Roughness` (0.05-1, rougher surfaces fall back to SSR / environment),
`Reflection Bounces` (1-2). Shared: `RT Resolution Scale`, `RT Denoiser`.
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md).

## Definition

A reflection shows what a surface "sees" in its mirror direction. RT
reflections trace that direction into the full 3D scene. Screen-space
reflections (SSR, owned by the reflections thread) can only reflect what is
already on screen, so they break when the reflected object is off screen,
behind something, or behind the camera. RT reflections have none of those
holes.

## How it works

1. From the G-buffer: world position `P`, normal `N`, roughness `α`,
   view vector `V = normalize(camPos - P)`.
2. Pick a reflection direction:
   - Mirror (α ≈ 0): `R = reflect(-V, N)`.
   - Glossy: importance-sample the GGX distribution of visible normals
     (**VNDF**, Heitz 2018) to get a microfacet normal `H`, then
     `R = reflect(-V, H)`. Rough surfaces spray rays over a wide lobe.
3. Trace a **closest-hit** ray from `P + N*bias` along `R`.
4. **Hit**: interpolate the hit triangle's normal and material with the
   barycentrics, then shade it: sun light (+ an RT shadow ray from the hit
   point if budget allows), emissive, and a cheap ambient term. Store the
   radiance and the **hit distance**.
5. **Miss**: sample the sky/environment.
6. Denoise. The hit distance drives the blur radius: the reflection of a
   nearby object on a rough floor is sharp at contact and blurs with distance.
7. Composite: `color += specularReflectance(F0, N, V, α) * reflection`, where
   the specular reflectance uses the split-sum environment BRDF so energy
   matches the rest of the lighting.

## Breakdown

| Step | Cost (1080p, compute BVH) | Notes |
|---|---|---|
| Classification | ~0.05 ms | Skip sky and pixels with α > max roughness |
| Trace + shade hit, half res | 1.0-2.5 ms | Most expensive part |
| Shadow ray at hit (optional) | +0.3-0.6 ms | Makes reflected shadows correct |
| Temporal + spatial denoise | 0.3-0.6 ms | Roughness-aware |
| Upsample + composite | ~0.1 ms | |

## Optimization

- **Roughness cutoff**: above ~0.5-0.6 roughness the reflection is so blurry
  that a pre-filtered environment probe or SSR is indistinguishable. Only
  trace below the cutoff. This is the biggest saving.
- **Half resolution, checkerboard, or 1 ray per 2x2 quad**, then upsample.
- **Hybrid SSR → RT**: march SSR first; only trace a ray when SSR fails
  (offscreen / occluded). Cuts ray count by 50-80% in many scenes.
- **Simplified hit shading**: no normal maps, a lower texture mip, no RT shadow
  at the hit (use the shadow map if available), no secondary reflections.
- **Mirror-ray reuse**: for very smooth surfaces 1 ray is exact, so skip the
  temporal blur to avoid ghosting.
- **Hit-distance-aware denoise** (NVIDIA ReLAX/REBLUR style) so contact
  reflections stay crisp.
- **Bounces**: second bounce costs another full trace for little visual gain;
  default 1.

## In this app

Code: `shaders/raytracing/rt_reflections.comp` → denoiser → `lighting.indirectSpecular` (a = confidence, fades out near the roughness cutoff).
Shared code: `src/modules/raytracing/bvh.cpp` (BVH), `shaders/raytracing/rt_common.glsl` (traversal, sampling),
`rt_gbuffer.glsl` (G-buffer reads).

### Measured cost

Measured with `gfxlab --bench-settings` on **Mesa llvmpipe 25.2 (software rasterizer, LLVM 20, 256-bit)**, 640×360, procedural demo scene, High preset baseline (RT AO and RT reflections on, RT resolution 0.75). Each value is the 60-frame average after the change minus the one before it. On llvmpipe the "GPU" is the CPU, so GPU and CPU ms move together, and run-to-run noise is about ±3 ms. Treat these as **relative** costs; a real GPU is 10-100x faster.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB |
|---|---|---|---|---|
| `rt.reflections` | Off | -16.9 | -16.9 | -14.1 |
| `rt.reflections` | On | +4.3 | +4.6 | +0.0 |
| `rt.reflections.rays` | 1 | -0.6 | -1.0 | +0.0 |
| `rt.reflections.rays` | 2 | -2.2 | -2.4 | +0.0 |
| `rt.reflections.max_roughness` | 0.05 | +0.3 | -0.4 | +0.0 |
| `rt.reflections.max_roughness` | 0.60 | +1.3 | +0.9 | +0.0 |
| `rt.reflections.max_roughness` | 1.00 | -0.7 | -1.1 | +0.0 |
| `rt.reflections.bounces` | 1 | -1.7 | -2.7 | +0.0 |
| `rt.reflections.bounces` | 2 | -2.4 | -3.4 | +0.0 |

RT reflections are in the baseline: turning them off saves ~16.9 ms and 14.1 MB.

## Rebuild (from scratch)

1. BVH + `traceClosest` returning `t`, triangle id and barycentrics.
2. RGBA16F texture: rgb = reflected radiance, a = hit distance.
3. Compute pass: reconstruct `P/N/α`; skip if α > cutoff; VNDF sample → `R`;
   `traceClosest`; on hit fetch the 3 vertex normals + material, interpolate,
   shade with the sun + emissive + ambient; on miss sample sky; write.
4. Temporal accumulation reprojected by the **reflected** point's motion
   (virtual position `P + R * hitDist` gives a better reprojection than the
   surface's own motion), alpha scaled by roughness.
5. Spatial à-trous filter with weights from depth, normal, roughness, and a
   radius that grows with hit distance and roughness.
6. Composite with the environment BRDF.
