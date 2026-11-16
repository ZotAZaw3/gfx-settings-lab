# Variable Rate Shading (VRS)

> Menu: **Performance → Variable Rate Shading** (+ debug: VRS Overlay)
> Setting ids: `performance.vrs` (Off / Quality / Balanced / Performance), `performance.vrsOverlay` · Owner module: `geometry.vrs`
> Presets: Low=Performance, Medium=Balanced, High=Off, Ultra=Off
> Files: `src/modules/geometry/vrs.cpp`, `shaders/geometry/vrs_rate.comp`, `vrs_apply.frag`

## Definition

VRS lets the GPU run the pixel shader **once per 2×2 or 4×4 block of pixels** in some
areas of the screen instead of once per pixel, while still rasterizing depth and edges at
full resolution. Detail is reduced only where you will not notice: dark areas, flat
surfaces, motion-blurred areas, the edges of the screen, or behind the HUD. It is a
DirectX 12 Ultimate / Vulkan feature (`D3D12_SHADING_RATE`, `VK_KHR_fragment_shading_rate`);
on OpenGL it is the vendor extension `GL_NV_shading_rate_image`.

## How it works

Three tiers in D3D12:
- **Per-draw (Tier 1)**: `RSSetShadingRate(2x2)` for a whole draw call, e.g. the skybox
  or distant terrain.
- **Per-primitive (Tier 2)**: the vertex/geometry shader outputs `SV_ShadingRate`.
- **Screen-space image (Tier 2)**: a small R8_UINT texture where each texel covers a
  tile (8×8 or 16×16 pixels) and stores that tile's rate (1×1, 1×2, 2×1, 2×2, 2×4, 4×2, 4×4).

Generating the shading rate image (each frame, compute shader):
1. Take last frame's final colour (reprojected) and compute **luminance contrast** per tile.
2. Low contrast (smooth sky, dark shadow) → coarse rate; high contrast (text, edges, foliage) → 1×1.
3. Combine with **motion**: fast-moving tiles get coarser (motion blur hides it).
4. Combine with **foveation** / screen edge / under the HUD.

The thresholds depend on the setting: Quality only goes to 2×2 in very flat tiles;
Performance allows 4×4 widely.

A **combiner** decides how per-draw, per-primitive and image rates merge (passthrough,
override, min, max).

## Breakdown

**In this prototype:**

1. Stage `Translucent`, order 1 (right after lighting wrote `scene.color`): "VRS rate image" compute builds a
   R32UI image, one texel per 16×16 tile, from luminance contrast (thresholds 0.02 / 0.05 / 0.09) and motion
   (`scene.velocity`, tiles moving > 12 / 6 px get one rate coarser).
2. "VRS apply" broadcasts one shaded sample per 2×2 / 4×4 block, which is exactly what hardware VRS does to the
   pixel shader's output. **This is an emulation**: the rhi (GL 4.5 core / D3D11) has no shading-rate API, so you see
   the image-quality cost but not the speed-up. The measured cost below is the emulation's overhead.


| Mode        | Max coarse rate | Contrast threshold | Typical pixel-shader saving |
|-------------|-----------------|--------------------|-----------------------------|
| Off         | 1×1             | –                  | 0 %                         |
| Quality     | 2×2             | very low           | 10–20 %                     |
| Balanced    | 2×2 (+2×4)      | low                | 20–35 %                     |
| Performance | 4×4             | medium             | 30–50 %                     |

Only **pixel-shader cost** is saved. Geometry, depth, bandwidth for render-target
writes and post-processing are unchanged. So VRS helps heavy forward/G-buffer shaders
most, and does little in a geometry- or CPU-bound scene. Artefact: blocky texture
detail in coarse tiles, most visible on high-frequency textures in motion.

## Optimization

- Drive the rate from **content** (luminance contrast + motion), not from fixed regions.
- Never coarsen UI, text or alpha-tested foliage edges.
- Pair with TAA / upscalers: their accumulation hides VRS blockiness. Generate the VRS
  image from the upscaler's history if available.
- Use 16×16 tiles if the hardware supports it (cheaper to generate, coarser control).
- For compute-based deferred lighting (which VRS hardware does not touch), implement
  **software VRS**: shade one pixel per 2×2 in the lighting compute shader for tiles the
  rate image marks as coarse, then broadcast.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers.

<!-- bench: performance.vrs -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Variable Rate Shading | Off | -11.433 | -13.737 | -13.662 | +0.0 | +0.0 |
| Variable Rate Shading | Quality | +4.055 | +2.208 | +2.202 | +0.0 | +0.3 |
| Variable Rate Shading | Balanced | -4.749 | -4.957 | -4.981 | +0.0 | +0.3 |
| Variable Rate Shading | Performance | -18.409 | -25.016 | -25.027 | +0.0 | +0.3 |
<!-- /bench -->

## Rebuild (from scratch)

Shading rate image generation:
```glsl
// vrs_image.comp — one workgroup per 16x16 tile
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding=0) uniform sampler2D uPrevColor;
layout(r8ui, binding=1) uniform writeonly uimage2D uRate;
uniform float uThreshold;  // from the setting
shared float sMin[256], sMax[256];
void main(){
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    float l = dot(texelFetch(uPrevColor, p, 0).rgb, vec3(0.2126,0.7152,0.0722));
    l = l / (1.0 + l);                                     // tonemap so HDR highlights don't dominate
    uint i = gl_LocalInvocationIndex; sMin[i] = l; sMax[i] = l; barrier();
    for (uint s = 128u; s > 0u; s >>= 1) { if (i < s) { sMin[i]=min(sMin[i],sMin[i+s]); sMax[i]=max(sMax[i],sMax[i+s]); } barrier(); }
    if (i == 0u) {
        float c = sMax[0] - sMin[0];
        uint rate = c < uThreshold*0.5 ? 2u : (c < uThreshold ? 1u : 0u); // 0=1x1 1=2x2 2=4x4
        imageStore(uRate, ivec2(gl_WorkGroupID.xy), uvec4(rate));
    }
}
```
Hardware use (NVIDIA GL): `glBindShadingRateImageNV(tex)`, set the palette with
`glShadingRateImagePaletteNV`, `glEnable(GL_SHADING_RATE_IMAGE_NV)` around the G-buffer pass.
D3D12: `cmdList5->RSSetShadingRateImage(rateTex)` with `D3D12_SHADING_RATE_COMBINER_OVERRIDE`.

Without hardware support the prototype runs in **emulation/visualization mode**: the
G-buffer/forward shader reads the rate image and, for coarse tiles, snaps its lighting
inputs to the top-left pixel of the 2×2/4×4 block, so you can see the visual effect
(and the overlay shows which tiles are coarse), though without the speedup.
