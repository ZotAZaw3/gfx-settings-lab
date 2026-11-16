# Resizable BAR / Smart Access Memory (info: system setting)

> Menu: not exposed; detected/hinted only · Owner: core

## Definition
Resizable BAR (PCIe "Base Address Register") lets the CPU address the GPU's **entire** VRAM instead of a
256 MB window. AMD calls it Smart Access Memory. It's enabled in the motherboard BIOS and GPU driver, not
in games, but several games are known to gain (or lose) a few percent with it, which is why NVIDIA keeps a
per-game whitelist.

## How it works
Without ReBAR, CPU writes into VRAM (vertex/constant/upload data) go through a 256 MB aperture or through a
staging copy in system RAM followed by a GPU copy. With ReBAR, the driver/app can place dynamic buffers
directly in VRAM and write them from the CPU across PCIe, removing copies. It helps workloads that stream
a lot of CPU-written data per frame (many dynamic constant buffers, streaming geometry/textures).

## Breakdown in this engine
- Per-frame CPU→GPU traffic here: `DrawConstants` ring (≤ 4 MB), FrameConstants, light SSBO, and texture
  streaming uploads (`textures.uploadBufferMB`). GL/D3D11 drivers decide placement themselves; with
  ReBAR they may put `GL_DYNAMIC_STORAGE_BIT` buffers in CPU-visible VRAM.
- D3D12/Vulkan: query `D3D12_HEAP_TYPE_GPU_UPLOAD` (Agility SDK) or a memory type that is both
  `DEVICE_LOCAL` and `HOST_VISIBLE` with a size > 256 MB to detect it.

## Optimization
- Typical effect: 0–10 % in CPU→GPU-heavy games; occasionally negative (hence whitelists).
- Pair with large upload budgets (streaming buffer size) to benefit; small budgets won't notice.

## Rebuild
1. Detect a large device-local + host-visible heap.
2. Place per-frame dynamic buffers there; write directly; keep a fallback staging path.
3. A/B test per title.
