# Ray-Traced Shadows

Settings (Ray Tracing tab): `RT Shadows` (`rt.shadows`), `Shadow Rays per Pixel` (1-4),
`Sun Angular Radius (deg)` (0-3, 0 = hard; the real sun is 0.27). Denoising uses the
shared `RT Denoiser` setting; resolution uses `RT Resolution Scale`.
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md).

## Definition

A pixel is in shadow if something blocks the straight line between it and the
light. RT shadows test that line directly with a ray, instead of comparing
against a depth map rendered from the light (shadow mapping, owned by the
shadows thread).

What you get over shadow maps:

- **No resolution limit**: no aliasing, no shimmering, no cascade seams.
- **No bias tuning**: no shadow acne, no peter-panning.
- **Physically correct soft shadows**: penumbra is sharp at contact and widens
  with distance from the occluder (contact hardening), automatically.
- **Tiny geometry casts shadows** (fences, wires, leaves) that would fall
  between shadow-map texels.

## How it works

For every on-screen pixel:

1. Read depth and normal from the G-buffer, reconstruct world position `P`.
2. Pick a direction to the light:
   - **Directional (sun)**: the sun is a disc of angular radius `θ`
     (real sun ≈ 0.27°). Sample a random point in a cone of that half-angle
     around `L`. Wider cone = softer shadow.
   - **Point/spot light of radius r**: sample a random point on a sphere of
     radius `r` at the light position.
3. Offset the origin along the normal (`P + N * 0.01 * dist`) so the ray
   doesn't hit the surface it starts on.
4. Trace an **occlusion ray** (`traceAny`) up to the light distance
   (`1e30` for the sun).
5. Visibility = 0 if hit, 1 if not. Average N rays.
6. Denoise (see [denoising.md](denoising.md)), then the lighting pass
   multiplies the direct light term by this visibility.

```glsl
vec3 L = sampleCone(sunDir, sunAngle, rand2(pixel, frame));
float vis = traceAny(P + N * bias, L, 1e30) ? 0.0 : 1.0;
```

Why 1 ray gives a noisy but unbiased result: each pixel sees a random point on
the sun. Across many pixels/frames, the average equals the fraction of the
sun disc that is visible, which is exactly what a penumbra is.

## Breakdown

| Step | GPU cost (1080p, compute BVH) | Output |
|---|---|---|
| Ray gen + trace, 1 rpp | ~0.6-1.2 ms | R8 visibility (noisy) |
| Temporal accumulation | ~0.1 ms | Stable visibility |
| Spatial blur (2 passes, depth/normal aware) | ~0.2 ms | Clean visibility |
| Composite | in lighting pass | — |

The visibility texture is single channel, so the denoiser is cheap compared to
reflections or GI.

## Optimization

- **One ray per pixel + temporal accumulation** is the standard. 4 rpp is only
  for screenshots.
- **Skip rays for back-facing pixels** (`dot(N, L) <= 0`): they're in shadow
  anyway. This alone saves ~30-50% on typical scenes.
- **Half-resolution tracing** with a depth-aware upsample. Shadows are low
  frequency in the penumbra and the hard edge is recovered by the upsample.
- **Hybrid**: use the shadow map (or screen-space contact shadows) for the
  sun and spend rays only where the shadow map is uncertain, i.e. penumbra
  pixels (`shadowmap PCF result in (0.05, 0.95)`). Used by several shipped games.
- **Penumbra-sized blur**: blur radius proportional to
  `(dReceiver - dBlocker) / dBlocker * sunAngle` (store hit distance). Contact
  edges stay sharp, distant ones blur more.
- **Per-light budget**: only the sun and the N nearest/brightest local lights
  get rays; the rest use shadow maps or no shadow.

## In this app

Code: `shaders/raytracing/rt_shadows.comp` → denoiser → `lighting.sunVisibility` (read by the lighting resolve instead of the shadow maps).
Shared code: `src/modules/raytracing/bvh.cpp` (BVH), `shaders/raytracing/rt_common.glsl` (traversal, sampling),
`rt_gbuffer.glsl` (G-buffer reads).

Measured cost (Δ GPU ms / Δ CPU ms / Δ VRAM / Δ RAM per value at 1920×1080) is filled in from
`gfxlab --bench-settings` once the app builds; cloud numbers come from Mesa llvmpipe (software)
and only show relative cost.

## Rebuild (from scratch)

1. Have the BVH + `traceAny` from the overview.
2. Create an `R8_UNORM` (or `R16F`) visibility texture at render resolution.
3. Compute shader, 8x8 groups: reconstruct `P`, `N`; early-out for sky and
   back-facing; sample a cone direction with a per-pixel, per-frame random
   (hash of pixel + frame index, or blue noise); offset origin; `traceAny`;
   write visibility.
4. Temporal pass: reproject with the motion vector / previous view-projection,
   blend `lerp(history, current, 0.1)`, reject history when depth differs by
   more than a few percent.
5. Two passes of a 5x5 à-trous blur weighted by depth and normal similarity.
6. In the lighting shader: `direct *= visibility` (replace the shadow-map
   lookup when RT shadows are on).
