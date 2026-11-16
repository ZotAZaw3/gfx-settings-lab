# Dynamic Resolution Scaling (DRS)

> Menu: **Display → Render Resolution → Dynamic Resolution** · Setting ids: `display.dynamicRes`,
> `display.dynamicRes.targetFps`, `display.dynamicRes.min`, `display.dynamicRes.max` · Owner: core
> Related: [render_scale.md](render_scale.md)

## Definition
Dynamic resolution automatically lowers the render resolution when the game can't hold the target frame
rate and raises it again when there is headroom. You get a steady frame rate in heavy scenes in exchange
for a softer image only while those scenes are on screen. Consoles use it almost universally.

## How it works
A feedback controller runs every 15 frames:
- Measure the recent average frame cost: `cost = max(GPU ms, CPU ms)`.
- The target is `1000 / targetFps` ms. Pixel cost is roughly proportional to pixel count, i.e. to
  `scale²`, so the ideal scale change is `sqrt(target / cost)`.
- Damping: the change per step is clamped to −10 % / +5 % (drop fast to avoid stutter, rise slowly to
  avoid oscillation), and the result is clamped to [Min Scale, Max Scale].
- Changes smaller than 2 % are ignored to avoid constant target reallocation.
- The DRS factor multiplies the user's Render Resolution (or the temporal upscaler's scale).
If the game is CPU-bound, lowering resolution doesn't help; the controller then sits at Min Scale.
That is a known limitation of the simple max(cpu, gpu) controller; production engines use GPU time only.

## Breakdown
1. `Engine::frame()` end → `updateDynamicResolution()` (every 15 frames) adjusts `dynamicScale_` and sets
   `requestResize`.
2. Next frame `handleResize()` recomputes `renderSize` and resizes render-res targets.

## Optimization
- This prototype reallocates render targets on every scale change (simple, costs a hitch of a few ms).
  Shipping engines allocate targets at the **maximum** scale once and render into a sub-rectangle
  (viewport) so DRS changes are free; all render-res passes then clamp UVs to the active rectangle.
- Temporal upscalers hide resolution changes much better than spatial upscaling.
- Use GPU timestamps (not frame time) as the input when V-Sync is on, or the controller sees the vsync
  wait as cost.

<!-- bench: display.dynamicRes -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Dynamic Resolution | Off | +3.093 | +3.118 | +3.225 | +0.0 | +0.0 |
| Dynamic Resolution | On | -12.680 | -16.356 | -16.189 | -6.2 | +9.3 |
<!-- /bench -->

## Rebuild
1. Measure GPU frame time with timestamp queries (read N frames later to avoid stalls).
2. Every K frames: `scale *= clamp(sqrt(target/measured), 0.9, 1.05)`; clamp to [min, max].
3. Either resize targets (simple) or render into a viewport of a max-size target (fast).
4. Feed the scale to the upscaler (and the mip bias).
5. Verify: set a target above what the GPU can do and watch the overlay's render size drop until the
   frame time meets the target.

Files: `src/core/engine.cpp` (`updateDynamicResolution`)
