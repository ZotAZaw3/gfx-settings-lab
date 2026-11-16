# Render Resolution (Render Scale) and Upscale Filter

> Menu: **Display → Render Resolution** · Setting ids: `display.renderScale` (25–200 %), `display.upscaleFilter`
> Presets: render scale 75 / 85 / 100 / 100 % · upscale filter Bilinear / Catmull-Rom / Catmull-Rom / Catmull-Rom
> Related: [dynamic_resolution.md](dynamic_resolution.md), [upscaling.md](upscaling.md) (temporal upscalers)

## Definition
Render Resolution draws the 3D scene at a percentage of your screen resolution and then stretches it
to fit, while the menu and HUD stay sharp at full resolution. 50 % at 1080p renders 960×540: a quarter
of the pixels, so pixel-heavy effects get up to ~4× cheaper, at the cost of a softer image. Above 100 %
the game **supersamples**: it renders more pixels than the screen has and averages them down, which
is the most expensive and most robust form of anti-aliasing.

## How it works
- `renderSize = outputSize × scale`. Everything before the `Upscale` stage (G-buffer, lighting,
  shadows sampled per pixel, SSAO, translucency, DOF…) runs at `renderSize`; everything after (bloom,
  tone mapping, UI) runs at `outputSize`.
- If a temporal upscaler (TAA-U / FSR-style, see the AA module) is active, it owns the render scale via
  its quality mode and reconstructs detail from previous frames. Otherwise core runs a **spatial**
  upscale with the selected filter:
  - **Nearest**: copy the closest pixel. Blocky, but no blur (pixel-art look).
  - **Bilinear**: blend the 4 nearest pixels. Smooth, soft.
  - **Catmull-Rom (bicubic)**: a 4×4 cubic kernel with slightly negative lobes, sharper than bilinear
    with mild ringing; implemented with 9 bilinear taps instead of 16 point taps.
- Texture LOD bias: when rendering below native resolution, textures would pick blurrier mips (the
  screen-space derivatives are larger). Core passes `log2(scale)` in `FrameConstants.display2.z`; the
  material sampler/upscaler can apply it so texture detail matches the output resolution.

## Breakdown
1. `Engine::updateRenderSize()`: `scale = upscalerRenderScale > 0 ? upscalerRenderScale : renderScale/100`,
   × dynamic resolution factor, clamped to [0.25, 2].
2. Changing the setting (`SettingRequiresResize`) → `RenderTargets::resize` recreates every
   render-res target and every module's `resize()` runs.
3. Stage `Upscale`, pass "Upscale (render scale)" (order 1000): if no module swapped the post chain,
   sample `post.src()` (render res) with the selected filter into `post.dst()` (output res).

| Resource | Format | Size |
|---|---|---|
| All `scene.*`, `gbuffer.*` | various | render res |
| `post.outputA/B`, `final.output` | RGBA16F | output res |

## Optimization
- GPU cost of pixel-bound work scales with `scale²`: 70 % ≈ half the pixels; 141 % ≈ double.
- CPU cost (draw calls, culling) does not change: render scale only helps when you are GPU-bound.
- VRAM for render-res targets scales with `scale²` too (≈ 35 bytes/pixel for the core G-buffer + HDR).
- Spatial upscaling cannot invent detail. Temporal upscalers at the same internal resolution look much
  better, which is why games pair render scale with TAA-U / FSR / DLSS / XeSS.
- Supersampling (>100 %) is the reference for AA quality, but 200 % costs 4× the pixels.

<!-- bench: display.renderScale display.upscaleFilter -->
<!-- /bench -->

## Rebuild
1. Keep two sizes: `outputSize` (swapchain) and `renderSize = round(outputSize × scale)`.
2. Allocate every scene target at `renderSize`; draw with a viewport of `renderSize`.
3. After the last render-res pass, run a fullscreen pass at `outputSize` sampling the scene color with
   the filter of your choice (bilinear sampler, or a bicubic kernel).
4. Draw the UI at `outputSize` after the upscale so text stays crisp.
5. Apply `mipLodBias = log2(renderWidth / outputWidth)` to material samplers.
6. Verify: 50 % should look 2× pixelated in Nearest mode, GPU time of pixel passes ≈ ¼.

Files: `src/core/engine.cpp` (`updateRenderSize`), `src/core/core_passes.cpp` (Upscale pass), `shaders/core/upscale.frag`
