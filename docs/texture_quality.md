# Texture Quality

> Menu: **Textures → Texture Quality** · Setting id: `textures.quality` · Module: `core.textures`
> Values: Low / Medium / High / Ultra · Presets: Low=Low, Medium=Medium, High=High, Ultra=Ultra

## Definition
Texture Quality sets the highest texture resolution the game keeps in video memory. Ultra shows
every texture at its authored resolution (crisp brick edges, readable text, fine fabric weave); lower
settings use half- or quarter-resolution copies so the game fits in GPUs with less VRAM. It rarely
changes frame rate much, but it is the setting most likely to cause stutter or blurry textures when it
asks for more memory than the GPU has.

## How it works
Every texture is stored as a **mip chain**: the full image (mip 0) followed by copies at 1/2, 1/4,
1/8 … resolution down to 1×1. A full chain costs 4/3 of the top level. The GPU picks a mip per pixel
from the screen-space derivative of the texture coordinates, so a wall far away samples a small mip.

Texture Quality is a **cap on the top mip**: the engine never uploads mips larger than the cap.

| Value | Max dimension kept | Mips dropped from a 4096² texture | VRAM for one 4096² RGBA8 texture |
|---|---|---|---|
| Low | 512 | 3 | 1.3 MB |
| Medium | 1024 | 2 | 5.3 MB |
| High | 2048 | 1 | 21 MB |
| Ultra | full | 0 | 85 MB |

Each step halves the resolution and quarters the memory. Because the GPU already uses small mips for
distant surfaces, the difference is only visible on surfaces close to the camera, where the texture
would be magnified.

Real games also choose per-texture-group caps (characters keep higher resolution than ground
clutter), and many use block compression (BC1/BC3/BC5/BC7: 4–8 bits per texel instead of 32), which
is independent of this setting. This prototype keeps RGBA8 to stay simple; the `rhi::Format::BC*`
formats exist for a compressed path.

## Breakdown
1. Load: `Scene::loadGltf` decodes each image with stb_image to RGBA8 and builds the full mip chain on
   the CPU (`buildMips`, box filter, sRGB textures filtered in linear space so mips don't darken).
2. Every frame, `TextureStreamingModule::update` (before any pass):
   - computes the quality cap → `qualityTop[t]` = first mip whose size ≤ cap;
   - combines it with the streaming estimate (see [texture_streaming.md](texture_streaming.md)):
     `wanted = max(streamingNeed + bias, qualityTop)`;
   - if `wanted != residentTopMip`, creates a GPU texture of size `width >> wanted` with
     `mipCount - wanted` levels and uploads those mips (`makeResident`).
3. Draws sample `SceneTexture::gpu` through the material sampler (texture filtering setting), so
   nothing in the shaders knows the quality level.

| Resource | Format | Size | Notes |
|---|---|---|---|
| Scene textures | RGBA8 / RGBA8_SRGB | `width >> top` | full mip tail below the cap |
| CPU mip chains | RGBA8 | full | kept in RAM as the streaming source |

## Optimization
- **Cost is memory, not shading.** Sampling a larger mip costs slightly more texture-cache bandwidth
  only when it is actually magnified on screen. GPU time barely moves; VRAM moves by 4× per step.
- When VRAM overflows, the driver pages textures over PCIe (D3D11/GL) or the game must evict
  (D3D12/Vulkan): that shows up as **hitches**, not lower average FPS. Pick the highest value whose
  "Δ VRAM" fits your card with ~15% headroom.
- Mip chains + trilinear/anisotropic filtering keep low settings from shimmering; dropping mips never
  causes aliasing, only blur.
- Compression is the bigger lever in shipped games: BC7 is 4× smaller than RGBA8 at nearly the same
  quality, so "Ultra" with BC7 costs what "High" costs uncompressed.

<!-- bench: textures.quality -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Texture Quality | Low | -0.772 | -1.671 | -1.762 | -16.0 | +0.0 |
| Texture Quality | Medium | -0.206 | -0.514 | -0.609 | +0.0 | +2.9 |
| Texture Quality | High | +2.271 | +2.067 | +2.142 | +0.0 | +2.9 |
| Texture Quality | Ultra | +3.672 | +5.363 | +5.358 | +0.0 | +2.9 |
<!-- /bench -->

The procedural scene has five 1024² textures and one 256² texture, so Medium/High/Ultra are
identical there (the cap never bites); load a glTF with 2K–4K textures (e.g. Sponza) to see the steps.

## Rebuild
1. At import, generate a full mip chain per texture (or load pre-built mips from DDS/KTX2). Filter
   sRGB color in linear space; renormalize normal-map mips.
2. Keep the source mips somewhere you can re-read cheaply (RAM, a memory-mapped pack file, or disk).
3. Expose a cap `maxDim` per quality level. When creating the GPU texture, skip mips whose dimension
   exceeds the cap: `top = 0; while (max(w>>top, h>>top) > maxDim) ++top;` and allocate
   `levels = mipCount - top` at size `w>>top`.
4. On change, recreate the texture (or, on D3D12/Vulkan, use a reserved/tiled resource and just
   commit/decommit the top mips). Materials keep their handle; swap the view.
5. Verify: VRAM counters drop by ~4× per step; at native res a 1:1 magnified surface gets blurrier one
   step at a time; distant surfaces look identical.

Files: `src/modules/core/texture_streaming.cpp`, `src/core/scene.cpp` (`buildMips`)
