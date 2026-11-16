# Texture Streaming (pool size, streaming buffer size)

> Menu: **Textures → Streaming** · Setting ids: `textures.streaming`, `textures.poolSizeMB`,
> `textures.uploadBufferMB`, `textures.streamingMipBias`, `textures.debugTint` · Module: `core.textures`
> Presets: Pool 128 / 256 / 512 / 1024 MB, Streaming buffer 4 / 8 / 16 / 32 MB per frame

## Definition
Texture streaming loads only the texture detail the camera can actually see. Instead of putting every
texture at full resolution in VRAM, the game keeps a **pool** of texture memory and fills it with the
mip levels that matter right now: close objects get sharp mips, distant or off-screen objects keep
small ones. Two settings control it:

- **Streaming Pool Size**: how much VRAM the streamer may use. Too small and textures near the camera
  stay blurry; too big and the game competes with everything else for VRAM.
- **Streaming Buffer Size** (a.k.a. "buffered streaming size", upload heap, staging budget): how many MB
  of new texture data may be copied to the GPU per frame. Small values avoid frame hitches but make
  detail "pop in" later; large values load faster but can spike frame time when you turn quickly.

## How it works
Each frame the streamer answers three questions:

1. **What mip does each texture need?** For every visible draw, estimate how many pixels the object
   covers: `projectedPx = radius / (distance · tan(fov/2)) · screenHeight`. The texture is spread over
   `uvRepeat` tiles, so it needs `projectedPx / uvRepeat` texels across, which means
   `mip = floor(log2(textureWidth / texelsNeeded))`. Take the minimum (sharpest) over every draw using
   the texture. Invisible textures keep a 16-pixel mip so they are never missing.
   (Engines like Unreal/Frostbite get the same answer more precisely with GPU **sampler feedback** or
   UV-density metadata computed at import time.)
2. **Does it fit the pool?** Sum the bytes of every wanted chain. While the sum exceeds the pool, raise
   the wanted mip (drop one level, ÷4 memory) of the **least important** texture (smallest projected
   size) first. That is what "texture streaming pool over budget" warnings in Unreal mean.
3. **What can be uploaded this frame?** Sort textures by importance, upload new chains until the
   per-frame **streaming buffer** budget is used; the rest wait for following frames (`pendingUploads`).
   Dropping detail (downgrades) is free and done immediately.

The Mip Bias asks for N levels blurrier than needed on top of all that (a cheap "low VRAM" mode).
Turning streaming **off** makes every texture fully resident at the Texture Quality cap.

## Breakdown
1. `Module::update` (before FrameSetup), uses last frame's draw list:
   - `estimate()` per draw item → `need[t]`, `importance[t]`;
   - `wanted[t] = clamp(max(need + bias, qualityTop), 0, lastMip)`;
   - fit to pool by raising `wanted` for low-importance textures;
   - `makeResident()` for changes within the upload budget.
2. Counters in the overlay: `textures.residentMB`, `textures.wantedMB`, `textures.uploadedMB/frame`,
   `textures.pendingUploads`, `textures.overBudget`. Enable **Visualize Resident Mips** (debug) for a
   per-texture table (resident vs wanted mip, VRAM).

| Resource | Where | Notes |
|---|---|---|
| Source mips | RAM (`SceneTexture::cpuMips`) | stands in for the disk pack file |
| Resident textures | VRAM, owner `textures` | recreated with `mipCount - top` levels |

## Optimization
- **Pool size** costs VRAM 1:1 and nothing else. Size it to what the visible set needs at your
  resolution (watch `textures.wantedMB` when you look around) plus ~20%.
- **Streaming buffer size** trades hitching for pop-in. Uploading 16 MB of RGBA8 per frame is ~1 ms of
  PCIe 4.0 transfer plus driver copy on desktop GPUs; consoles/SSDs with DirectStorage push far more
  because they decompress on the GPU (see [directstorage.md](directstorage.md)).
- Real streamers also: prefetch along the camera velocity, keep a "mip tail" (all mips ≤ 64 KB) always
  resident, use tiled/sparse resources so a texture's top mip can be added without recreating it, and
  read from compressed pack files on a worker thread. This prototype recreates the texture and uploads
  synchronously on the render thread, which is the simplest correct version and makes the cost visible
  in CPU time.
- Hitch source to watch: a texture whose single chain is larger than the per-frame budget is still
  uploaded in one go (the first upload each frame is always allowed so nothing starves).

<!-- bench: textures.streaming textures.poolSizeMB textures.uploadBufferMB textures.streamingMipBias -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Texture Streaming | Off | -7.675 | -3.780 | -3.476 | +5.0 | +5.3 |
| Texture Streaming | On | -4.000 | -3.626 | -3.645 | +0.0 | +5.3 |
| Streaming Pool Size | 16 MB | +4.878 | +2.946 | +2.957 | -8.5 | +0.0 |
| Streaming Pool Size | 512 MB | +2.253 | +1.433 | +1.523 | +0.0 | +0.0 |
| Streaming Pool Size | 4096 MB | -3.439 | -4.737 | -4.642 | +0.0 | +0.0 |
| Streaming Buffer Size | 0.5 MB/frame | +6.894 | +9.386 | +9.496 | +0.0 | +0.0 |
| Streaming Buffer Size | 16.0 MB/frame | +1.419 | +1.101 | +1.341 | +0.0 | +0.0 |
| Streaming Buffer Size | 128.0 MB/frame | +3.039 | +5.789 | +5.839 | +0.0 | +0.0 |
| Streaming Mip Bias | 0 | -6.663 | -0.296 | -0.595 | +0.0 | +0.0 |
| Streaming Mip Bias | 1 | -0.271 | +1.162 | +1.176 | -16.5 | +0.0 |
| Streaming Mip Bias | 2 | -4.233 | -0.086 | -0.374 | -20.6 | +0.0 |
| Streaming Mip Bias | 3 | -8.830 | -5.265 | -5.546 | -21.7 | +0.0 |
<!-- /bench -->

## Rebuild
1. Store textures with full mip chains in a format you can read per mip (DDS/KTX2 or your own pack).
2. Per texture keep: resident top mip, wanted top mip, importance, last-used frame.
3. Each frame compute the wanted mip from screen coverage (CPU estimate as above, or GPU sampler
   feedback / a low-res "feedback buffer" pass that writes texture id + mip per pixel and is read back
   a couple of frames later).
4. Sort by importance; fit into the budget by degrading the least important first; never evict what
   is needed for this frame's closest objects.
5. Upload with a fixed-size ring of staging buffers (that ring *is* the streaming buffer size); copy
   into the destination mip; on D3D12/Vulkan use a copy queue and fences so the main queue never waits.
6. Swap the texture view only when the new mips are fully uploaded, so the user never sees garbage.
7. Verify by shrinking the pool until `overBudget` turns on: distant/small textures should blur first,
   the object in front of the camera last.

Files: `src/modules/core/texture_streaming.cpp`
