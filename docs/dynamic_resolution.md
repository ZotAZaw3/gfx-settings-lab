# Dynamic Resolution Scaling (DRS)

> Menu: **Display → Dynamic Resolution (core)** · Setting id(s): core settings · Owner module: `core`
> Values: Off / On, target FPS, min/max scale · Presets: core

Settings shown in the menu: `Dynamic Resolution: Off | On`, `Target FPS`, `Min Scale %`, `Max Scale %`

## Definition

Dynamic resolution changes the internal render resolution every frame (or every few frames) to hold a target frame time. When the GPU is overloaded the scene renders at fewer pixels; when there's headroom it goes back up. Paired with a temporal upscaler the output stays at native resolution and the change is hard to notice. Console games use it almost universally.

## How it works

1. Measure GPU frame time with timer queries (`GL_TIME_ELAPSED` query objects / `D3D11_QUERY_TIMESTAMP`), read back 2-3 frames late to avoid stalls.
2. Compare to the budget (`1000 / targetFPS` ms, minus a safety margin ~5%).
3. Pixel cost ≈ proportional to pixel count, so the new scale is `scale_new = scale * sqrt(budget / gpuTime)`.
4. Smooth it (exponential moving average, or a PI controller) and clamp to [min, max]. Change upward slowly, downward quickly.
5. Render targets are allocated at max size; each frame only a viewport of `scale * output` is used (no reallocation). Shaders receive the active render size so UV math uses `renderSize / allocatedSize`.
6. The upscaler (or bilinear) resolves to output size.

## Breakdown

| Piece | Notes |
|---|---|
| GPU timer queries | ring of 4 queries, read the oldest |
| Controller | `scale *= clamp(sqrt(budget/time), 0.9, 1.05)`, then EMA |
| Viewport-in-oversized-target | avoids reallocations and history resets |
| UV clamp | samplers must clamp to the active sub-rectangle, not the texture edge |
| Upscaler input | render size changes per frame; jitter phase count updates too |

Code: render scale and dynamic resolution are implemented by the core (`src/modules/core/`). The upscaler's quality mode sets `Engine::upscalerRenderScale` (`src/modules/aa/temporal.cpp`) and DRS multiplies on top of it.

## Optimization

- Quantise the scale to steps of e.g. 1/16 to keep jitter sequences and caches stable.
- Hysteresis: only go up after N frames of headroom; go down immediately.
- Exclude the UI and post passes at output resolution from the measured time, since they don't scale.
- CPU-bound frames: DRS can't help; detect that (GPU time well below frame time) and stop lowering.

## Rebuild (from scratch)

```cpp
float budget = 1000.f / targetFps * 0.95f;
float gpuMs  = timer.latestResolvedMs();          // 2-3 frames old
float k      = std::sqrt(budget / std::max(gpuMs, 0.1f));
k            = std::clamp(k, 0.90f, 1.05f);
scale        = std::clamp(scale * k, minScale, maxScale);
scale        = std::round(scale * 16.f) / 16.f;
core.setRenderScale(scale);
```

Steps: add timer queries, the controller above, viewport-based render targets, clamp-to-subrect sampling, and a graph overlay plotting scale and GPU time.
