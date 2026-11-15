# ReSTIR (Reservoir-based Spatio-Temporal Importance Resampling)

Settings (Ray Tracing tab, "Advanced (documented)"): `ReSTIR DI (many lights)`, `ReSTIR GI`.
Both are greyed out in the prototype; the "?" opens this page.
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md),
[path_tracing.md](path_tracing.md).

## Definition

ReSTIR is a sampling technique, not an effect. It makes 1 ray per pixel
behave like dozens by **reusing good samples from neighbouring pixels and from
previous frames**. It's the reason "real-time path tracing" works in games:

- **ReSTIR DI** (Bitterli et al. 2020): direct lighting from thousands or
  millions of lights (neon signs, emissive triangles) with one shadow ray per
  pixel. NVIDIA RTXDI.
- **ReSTIR GI** (Ouyang et al. 2021): reuses one-bounce indirect samples
  (the hit point and its radiance) between pixels.
- **ReSTIR PT** (Lin et al. 2022): reuses whole paths. Used by Cyberpunk 2077
  RT Overdrive and Alan Wake 2 path tracing.

## How it works

The building block is **weighted reservoir sampling (WRS)**: stream through
candidates, keep one, with probability proportional to its weight, using O(1)
memory:

```
struct Reservoir { Sample y; float wSum; float M; float W; };
void update(inout Reservoir r, Sample x, float w, float u) {
    r.wSum += w; r.M += 1;
    if (u < w / r.wSum) r.y = x;
}
```

Per pixel per frame (ReSTIR DI):

1. **Initial candidates (RIS)**: draw N cheap light samples (e.g. 32) from a
   simple distribution (uniform or power-weighted). For each compute the
   *target* `p̂ = luminance(BRDF · Le · G)` **without** a shadow ray (cheap),
   weight `w = p̂ / sourcePdf`, stream into the reservoir.
2. **Visibility**: trace one shadow ray to the chosen sample. If occluded,
   zero the reservoir (prevents reusing blocked lights).
3. **Temporal reuse**: reproject to last frame's pixel, merge its reservoir
   (capping its `M` at ~20x current so stale samples don't dominate).
4. **Spatial reuse**: merge reservoirs from k random neighbours (radius
   ~30 px) whose depth/normal are similar. Each merge re-evaluates `p̂` at
   *this* pixel.
5. **Final weight**: `W = wSum / (M · p̂(y))`; shade with
   `f(y) · W` (and a final shadow ray, or reuse step 2's).

ReSTIR GI does the same with samples = (hit position, hit normal, outgoing
radiance) from a GI ray, and a Jacobian term to correct for the different
geometry when reusing a neighbour's hit.

Unbiased variants need extra shadow rays for neighbour visibility (or MIS
weights); biased variants skip them and are what most games ship.

## Breakdown

| Step | Cost (1080p) |
|---|---|
| 32 candidates, no rays | ~0.3-0.6 ms |
| 1 visibility ray | ~0.5-1 ms (compute BVH) |
| Temporal reuse | ~0.1 ms |
| Spatial reuse, 5 taps | ~0.3-0.5 ms |
| Final shading | ~0.1 ms |

Memory: 2 reservoir buffers (current + previous), ~32 bytes per pixel each.

## Optimization

- **Light BVH or alias table** for initial candidates instead of uniform.
- **Pre-sampled light tiles** (RTXDI): pick a few thousand lights per frame
  once, then all pixels draw from that small set (cache friendly).
- **Fewer spatial taps on the second pass**, or one spatial pass only.
- **Cap M** (history length) to adapt to moving lights.
- **Boiling filter**: discard reservoirs whose weight is far above their
  neighbours' (prevents blotchy artefacts).
- **Biased reuse** (no extra visibility rays) is fine with a denoiser after.
- Pair with a denoiser (NRD ReLAX is tuned for ReSTIR output) or with Ray
  Reconstruction.

## Rebuild (from scratch)

1. A light list (sun + emissive triangles + point lights) with power.
2. Two `Reservoir` SSBOs (ping-pong) sized width × height.
3. Pass 1 (candidates + visibility), pass 2 (temporal merge with reprojected
   previous reservoir), pass 3 (spatial merge), pass 4 (shade).
4. Swap reservoir buffers; denoise; composite.

Status in this app: documented. The prototype's path tracer uses NEE to the
sun only (one light), where ReSTIR DI has nothing to gain; the toggles appear
greyed out with a tooltip pointing here.
