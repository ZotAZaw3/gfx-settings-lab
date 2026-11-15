# Ray Reconstruction (DLSS RR) and Ray-Traced Denoising

> Menu: **Anti-Aliasing & Upscaling → Ray Reconstruction** · Setting id(s): `aa.ray_reconstruction` · Owner module: `aa.temporal`
> Values: Off / RR-lite / DLSS RR* (greyed out) · Presets: Off at every preset

Settings shown in the menu: `Ray Reconstruction (aa.ray_reconstruction): Off (classic: RT denoiser then TAA/upscaler) | RR-lite (ours: joint denoise + upscale) | DLSS RR*` (*D3D + SDK + RTX only)

The classic SVGF denoiser lives in the ray tracing module and is explained in [denoising.md](denoising.md). This page covers what Ray Reconstruction changes and our educational stand-in, RR-lite.

## Definition

Real-time ray tracing affords only ~0.5-2 rays per pixel, so raw ray-traced lighting (reflections, GI, shadows, AO) is extremely noisy. A **denoiser** turns that noise into a clean image using spatial filtering and temporal accumulation. Games traditionally ran *separate hand-tuned denoisers* per effect (NVIDIA NRD's ReBLUR/ReLAX/SIGMA) and *then* ran the upscaler. **Ray Reconstruction** (DLSS 3.5, 2023) replaces all those denoisers *and* the upscaler with one neural network that sees the noisy signal plus G-buffer guides, so it keeps more detail (no double blur) and reacts faster to lighting change.

## How it works

Classic pipeline (setting `Off`, see [denoising.md](denoising.md)): noisy RT signal → per-effect SVGF denoiser (temporal accumulation + variance + 5 à-trous passes) → remodulate → TAA or temporal upscaler. Two temporal filters and a spatial blur stacked, each losing detail and adding lag.

**RR-lite** (setting `RR-lite`, `shaders/aa/rr_lite.frag`), our educational stand-in for DLSS RR:

1. The RT module skips its denoiser and writes the raw signal `rt.noisy` (demodulated by albedo) plus `rt.albedo`.
2. One pass at **output** resolution does denoise and upscale together: for each output pixel, find the nearest render-res sample (jitter removed), run a tiny 3x3 depth/normal-guided gather on the noisy signal.
3. Reproject the output-res history with motion vectors. Reject it using **geometry guides** (previous depth and normal stored at output res), not a colour box: a noisy signal's neighbourhood colour range is huge and would let ghosting through or reject everything.
4. Accumulate as a capped running average (`1 / historyLength`, max 32-64 frames): converged regions approach a many-spp result at output resolution.
5. Remodulate by albedo at the end so texture detail stays sharp.

What it lacks compared to the real thing: no learned handling of moving specular highlights (it reprojects with surface motion, so mirror reflections lag), no hit-distance-aware filtering, and slower convergence after disocclusion. Those gaps are exactly what the neural network is trained to fill.

Ray Reconstruction (DLSS RR): inputs are the same noisy signal and guides, plus specular hit distance and roughness; a single network does the denoise + upscale at once. It is trained on offline path-traced references, so it handles cases where heuristics fail (moving specular reflections, shadow contact detail).

## Breakdown

| Pipeline | Passes | Cost 1080p out | Detail | Lag |
|---|---|---|---|---|
| Classic: SVGF + TAA/upscaler | temporal, variance, 5 à-trous, TAA | ~1.2 ms + TAA | soft (double blur) | 2 histories |
| RR-lite (ours) | 1 pass at output res | ~0.4 ms | sharp when static, noisy after disocclusion | 1 history |
| DLSS RR | 1 network inference | ~1.5-3 ms (tensor cores) | best | lowest |

Code: `src/modules/aa/temporal.cpp`, `shaders/aa/rr_lite.frag`. Inputs `rt.noisy` and `rt.albedo` come from the ray tracing module; when no RT effect is on, the setting is greyed out.

## Optimization

- Denoise at render resolution, before upscaling.
- Separate diffuse and specular signals: specular needs shorter history and roughness-aware filtering.
- Use the history length to reduce filter radius where history is long (fewer à-trous iterations needed).
- Reproject specular with the *virtual* (reflected) position for mirror-like surfaces.
- Pack guides (normal octahedral + depth) into one RGBA16F to cut bandwidth.

## Rebuild (from scratch)

Core of `rr_lite.frag`:

```glsl
vec2 rp  = outUV * renderSize - 0.5 + jitterPx;        // nearest render sample, jitter removed
ivec2 q0 = ivec2(floor(rp + 0.5));
vec3 cur = guidedGather3x3(rtNoisy, q0);               // depth/normal edge-stopping weights
vec2 prevUV = outUV - velocityToUV(velocity[q0]);
vec4 pg = texture(prevGuide, prevUV);                  // previous normal.xyz + depth.w at output res
bool ok = abs(pg.w - depth[q0]) < tol * depth[q0] && dot(pg.xyz, normal[q0]) > 0.85;
float n = ok ? min(history(prevUV).a + 1.0, 32.0) : 1.0;
vec3 acc = ok ? mix(history(prevUV).rgb, cur, 1.0 / n) : cur;
writeHistory(acc, n); writeGuide(normal[q0], depth[q0]);
out = acc * albedo[q0];                                 // remodulate
```

Steps: have the RT pass skip its denoiser in this mode, allocate output-res history + guide ping-pong targets, run the pass above in the Upscale stage instead of the normal upscaler, compare against the classic chain with the camera still (RR-lite converges sharper) and moving (shows its disocclusion noise).
