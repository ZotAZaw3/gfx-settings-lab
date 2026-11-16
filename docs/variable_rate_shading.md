# Variable Rate Shading (VRS)

Menu: **Performance → Variable Rate Shading** (`Off | Quality | Balanced | Performance`),
**VRS Debug Overlay** (on/off)
Code: `GeoVfxSettings::vrs`, `vrsOverlay`; `VrsController` (hardware path when
`GL_NV_shading_rate_image` exists, software emulation otherwise); shader `geometry/vrs_image.comp`

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
