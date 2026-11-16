# Exposure & Eye Adaptation (Auto Exposure)

> Menu: **Post Processing → Exposure** · Setting id(s): `post.exposure.auto`, `.compensation`, `.min`, `.max`, `.speedUp`, `.speedDown`, `.key`, `.center` · Owner module: `post`
> Values: Auto On/Off, compensation −5…+5 EV · Presets: not preset-driven (compensation is NoPreset)

## Definition

**Exposure** is a multiplier applied to the HDR scene before tonemapping, the
same job as a camera's aperture/shutter/ISO combination. **Eye adaptation**
(auto exposure) computes that multiplier automatically from how bright the
image currently is, and changes it gradually over time, imitating the pupil
and retina: walking out of a dark tunnel into sunlight blows out white for a
moment, then settles.

## How it works

1. **Measure scene luminance.** For every pixel compute
   `L = dot(rgb, vec3(0.2126, 0.7152, 0.0722))`. Average it as a *geometric*
   mean (average of `log(L)`) so one very bright sun pixel does not dominate:
   `Lavg = exp( mean( log(L + ε) ) )`.
2. **Reduce to one number.** This prototype writes `log(L)` into a 1/4-res
   `R16F` texture, then calls `glGenerateMipmap`; the smallest mip (1×1) is the
   mean. (A compute-shader histogram is the AAA approach, see Optimization.)
3. **Target exposure.** With middle-grey key `k` (0.18 by default):
   `targetExposure = k / Lavg`, then converted to EV, clamped to
   `[minEV, maxEV]`, and offset by the user's exposure compensation.
4. **Temporal smoothing.** The current exposure moves toward the target with
   an exponential that does not depend on frame rate:
   `ev += (targetEV - ev) * (1 - exp(-dt * speed))`. Using a different speed
   when getting brighter vs. darker reproduces how eyes adapt faster to light
   than to dark.
5. **Apply.** The tonemap pass multiplies scene color by `exp2(ev)` before
   tonemapping. The adapted value is stored in a 1×1 `R32F` texture that ping-
   pongs between two frames so it stays entirely on the GPU (no readback stall).

## Breakdown

| Piece | Where | Detail |
|---|---|---|
| Luminance pass | `shaders/post/luminance.frag` | writes `log2(L)` at 1/4 res, weighted toward the center (center-weighted metering) |
| Mip reduction | `glGenerateMipmap` | hardware box filter down to 1×1 |
| Adaptation pass | `shaders/post/adapt.frag` | reads 1×1 mip and last frame's EV, writes new EV |
| Apply | `shaders/post/tonemap.frag` | `color *= exp2(ev + compensation)` |
| Manual mode | settings | skips the luminance/adapt passes, uses compensation only |

## Optimization

- Measure at 1/4 or 1/8 resolution, luminance does not need detail.
- **Histogram (compute shader, 256 bins)**: build a log-luminance histogram
  in shared memory, then discard the darkest ~50% and brightest ~5% of pixels
  before averaging. This ignores the black sky and the sun, giving far more
  stable exposure. Costs ~0.05 ms at 1080p on a mid-range GPU.
- Never read the value back to the CPU (`glReadPixels` stalls the pipeline
  for a frame); keep it in a texture and sample it in the composite.
- Update the luminance every 2nd frame if needed; the temporal filter hides it.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.exposure.auto` | Off | +4.8 | +3.8 | +0.0 | n/a |
| `post.exposure.auto` | On | +0.1 | -0.5 | +0.0 | n/a |

## Rebuild (from scratch)

1. Create a 1/4-res `GL_R16F` texture with a full mip chain
   (`levels = 1 + floor(log2(max(w,h)))`) and `GL_LINEAR_MIPMAP_NEAREST`.
2. Fragment shader:
   ```glsl
   float L = dot(texture(uHDR, vUV).rgb, vec3(0.2126,0.7152,0.0722));
   out_log = log2(max(L, 1e-4));
   ```
3. `glGenerateMipmap(GL_TEXTURE_2D)`; then `textureLod(uLum, vec2(.5), maxMip).r`
   is the average `log2` luminance.
4. Two 1×1 `R32F` targets A and B. Adapt shader reads A, writes B:
   ```glsl
   float target = clamp(log2(uKey) - avgLog2, uMinEV, uMaxEV);
   float cur = texture(uPrev, vec2(.5)).r;
   float speed = target > cur ? uSpeedUp : uSpeedDown;
   outEV = cur + (target - cur) * (1.0 - exp(-uDt * speed));
   ```
   Swap A/B every frame.
5. In the final pass: `hdr *= exp2(ev + uCompensation)`.
6. D3D11: same, with `GenerateMips` on an SRV created with
   `D3D11_RESOURCE_MISC_GENERATE_MIPS`, or a compute shader histogram with
   `InterlockedAdd` on `groupshared uint bins[256]`.

Files: `src/modules/post/post_module.cpp` (runAdapt, runTonemap), `shaders/post/luminance.frag`, `shaders/post/adapt.frag`, `shaders/post/tonemap.frag`
