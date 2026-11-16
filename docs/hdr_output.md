# HDR Output (HDR10 / scRGB), Peak Brightness, Paper White, Black Level

> Menu: **Display → Brightness & HDR** · Setting ids: `display.hdr.mode` (SDR / HDR10 (PQ) / scRGB (FP16)),
> `display.hdr.peakNits`, `display.hdr.paperWhiteNits`, `display.hdr.blackLevelNits` · Owner: core
> Related: [hdr_calibration.md](hdr_calibration.md) (calibration screen, post module), [tonemapping_hdr.md](tonemapping_hdr.md), [gamma_brightness.md](gamma_brightness.md)

## Definition
An SDR monitor shows roughly 0–300 nits; HDR TVs and monitors reach 600–2000+ nits and much deeper
blacks. HDR output sends the game's bright highlights (sun, fire, specular glints) to the display as
genuinely brighter light instead of compressing everything into SDR white. The three sliders adapt it to
your screen:
- **Peak Brightness**: the brightest your display can actually show. Highlights are compressed to fit.
- **Paper White**: how bright a white UI element / a white sheet of paper in the scene looks; this sets
  the overall image brightness (203 nits is the ITU reference).
- **Black Level**: lifts the darkest values for displays or rooms where shadow detail gets lost.

## How it works
- The renderer is already HDR internally (RGBA16F). The difference is only at the end of the pipeline.
- **SDR**: tone map to [0,1], gamma-encode (see gamma doc).
- **HDR10**: tone map to *display-relative* values where 1.0 = paper white, convert Rec.709 primaries to
  Rec.2020, multiply by paper white to get **nits**, clamp to peak, then encode with the **PQ curve**
  (SMPTE ST 2084), an absolute curve where code value ↔ nits is fixed up to 10 000 nits. The swapchain is
  `R10G10B10A2` with color space `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`.
- **scRGB**: linear Rec.709 primaries in FP16 where 1.0 = 80 nits and values above 1 (or negative, for
  wide gamut) are allowed. The swapchain is `R16G16B16A16_FLOAT` with `RGB_FULL_G10_NONE_P709`; Windows
  composes it into whatever the display needs.
- The tone mapper (post module) must know peak and paper white so highlights roll off at the panel's
  real limit instead of clipping (it reads `FrameConstants.display.w` / `display2.x`).

## Breakdown
1. `display.hdr.mode` change → `Engine::applyDisplaySettings()` → `Device::setOutputColorSpace()`:
   - D3D11 (Windows): recreate the swapchain buffers in RGB10A2 or RGBA16F and call
     `IDXGISwapChain3::SetColorSpace1`; returns false if the output isn't HDR-capable.
   - GL: GLFW/WGL/GLX expose no HDR swapchain, so it returns false (SDR swapchain).
2. Stage `OutputTransform`: mode 1 → `pqEncode(min(Rec2020(c) × paperWhite, peak) + black)`;
   mode 2 → `c × paperWhite / 80`.
3. **Preview without an HDR swapchain** (GL, SDR monitors): the PQ signal is shown as-is, which looks
   flat and washed out. That is what HDR10 data looks like when a display interprets it as SDR, and is
   useful to see the encoding. scRGB previews clipped.

## Optimization
- Cost: one PQ encode (a few `pow`s) per output pixel, plus RGB10A2/FP16 swapchain bandwidth (FP16 doubles
  scanout/composition bandwidth vs 8-bit). See the bench table.
- Quality pitfalls: banding if you encode PQ into 8-bit; double tone mapping if the TV's own tone mapping
  (HGIG off) fights the game's; UI too bright if it isn't drawn at paper white.
- Use HGIG / "HDR game mode" on TVs so the game's peak setting is respected.

<!-- bench: display.hdr.mode display.hdr.peakNits display.hdr.paperWhiteNits -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| HDR Output | SDR | -4.020 | -7.840 | -7.882 | +0.0 | +0.0 |
| HDR Output | HDR10 (PQ) | -2.616 | -2.227 | -2.283 | +0.0 | +0.0 |
| HDR Output | scRGB (FP16) | +7.290 | +4.785 | +4.688 | +0.0 | +0.0 |
| HDR Peak Brightness | 400 nits | +2.652 | +2.057 | +2.053 | +0.0 | +0.0 |
| HDR Peak Brightness | 1000 nits | +5.421 | +6.705 | +6.716 | +0.0 | +0.0 |
| HDR Peak Brightness | 10000 nits | +3.426 | +4.439 | +4.543 | +0.0 | +0.0 |
| HDR Paper White | 80 nits | +2.197 | +4.815 | +4.829 | +0.0 | +0.0 |
| HDR Paper White | 203 nits | +3.071 | +1.158 | +1.066 | +0.0 | +0.0 |
| HDR Paper White | 500 nits | +4.052 | +8.972 | +8.868 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild
1. Render and post-process in linear FP16.
2. Tone map with peak/paper-white awareness (e.g. ACES or Reinhard extended with white = peak/paperWhite).
3. Output: HDR10 → BT.709→BT.2020 matrix, × paperWhite (nits), clamp peak, PQ encode, write to an RGB10A2
   swapchain with the PQ/2020 color space. scRGB → × paperWhite/80, write FP16 with the linear/709 color space.
4. Detect support: `IDXGIOutput6::GetDesc1()` → `ColorSpace == G2084_P2020` and `MaxLuminance` (use it as
   the default peak).
5. Draw UI at paper white, composite before encoding.
6. Verify with a PQ test pattern: 1000-nit patch must look brighter than the 203-nit patch on an HDR
   display, and identical when peak is set to 203.

Files: `shaders/core/output_transform.frag`, `src/core/engine.cpp` (`applyDisplaySettings`), D3D11 backend swapchain
