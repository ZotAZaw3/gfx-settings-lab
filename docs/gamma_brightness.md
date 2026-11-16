# Brightness and Gamma

> Menu: **Display → Brightness & HDR** · Setting ids: `display.brightness` (0.5–1.5), `display.gamma` (1.6–2.8, default 2.2) · Owner: core

## Definition
**Brightness** scales the final image up or down so dark scenes are readable on your screen and in your
room. **Gamma** sets the curve used to turn the renderer's linear light values into the numbers sent to
an SDR monitor: lower gamma lifts shadows and mid-tones (washed out if too low), higher gamma deepens
them (crushed blacks if too high). Most games show a "make the logo barely visible" calibration image
for this.

## How it works
The renderer works in **linear** light (doubling a value doubles the photons). Monitors expect
**gamma-encoded** values: roughly `signal = linear^(1/2.2)` (the sRGB standard is a near-2.2 curve
with a linear toe). Encoding spends more of the 8-bit code values on dark tones, where the eye is most
sensitive, which is why skipping it causes banding and a too-dark, contrasty image.

The output transform does, per pixel, after tone mapping:
```
c = toneMapped * brightness              // linear, 0..1 for SDR
signal = pow(clamp(c, 0, 1), 1 / gamma)  // display encoding
```
Brightness acts before the curve (a linear exposure-like gain); gamma reshapes the curve so the middle
grey moves while black (0) and white (1) stay fixed.

Textures follow the same rule in the other direction: color textures are stored sRGB-encoded and the
GPU decodes them to linear when sampling (`*_SRGB` formats); the `gbuffer.albedo` target is sRGB so it
gets 8-bit precision where it matters.

## Breakdown
1. Stage `ToneMap` (post module, or core ACES fallback): HDR → display-linear [0, 1].
2. Stage `OutputTransform`, pass "Output transform" (`shaders/core/output_transform.frag`):
   brightness × → gamma encode → `final.output` (RGBA16F).
3. `copyToBackbuffer()` copies to the (non-sRGB) swapchain image; the encoded values reach the panel unchanged.

## Optimization
- Essentially free: one `pow` per output pixel in a pass that runs anyway (see table).
- Do not combine an sRGB swapchain with manual gamma, or the image is encoded twice (very bright). This
  engine requests a linear swapchain and encodes in the shader so the gamma slider can work.
- Brightness/gamma are display calibration, not artistic grading; HDR output ignores gamma and uses
  paper white / peak nits instead (see [hdr_output.md](hdr_output.md)).

<!-- bench: display.brightness display.gamma -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Brightness | 0.50 | +1.934 | +3.289 | +3.004 | +0.0 | +0.0 |
| Brightness | 1.00 | +7.874 | +8.520 | +8.296 | +0.0 | +0.0 |
| Brightness | 1.50 | +3.971 | +6.254 | +5.938 | +0.0 | +0.0 |
| Gamma | 1.60 | -6.340 | -4.487 | -4.624 | +0.0 | +0.0 |
| Gamma | 2.20 | -3.933 | -2.573 | -2.651 | +0.0 | +0.0 |
| Gamma | 2.80 | -7.656 | -11.360 | -11.376 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild
1. Keep every lighting and post computation in linear space; sample color textures as sRGB.
2. Tone map to [0, 1] display-linear.
3. Final pass: `out = pow(saturate(c * brightness), 1/gamma)` into a UNORM (non-sRGB) swapchain (or use the
   exact sRGB curve and an sRGB swapchain when you don't need a gamma slider).
4. Give the player a calibration image: a near-black logo on black that should be barely visible.

Files: `shaders/core/output_transform.frag`, `src/core/core_passes.cpp`
