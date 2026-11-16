# DirectStorage / GPU Decompression (info, relates to Texture Streaming)

> Menu: not exposed · Related: [texture_streaming.md](texture_streaming.md) · Owner: core

## Definition
DirectStorage (Windows, D3D12) is an I/O API that reads game assets from NVMe SSDs with very little CPU
overhead and can **decompress them on the GPU** (GDeflate). It makes loading screens shorter and lets
streaming systems fetch high-resolution textures much faster, so detail pops in sooner while turning the
camera.

## How it works
Classic path: `ReadFile` → CPU decompresses (zlib/Oodle) → copies to an upload heap → GPU copy to VRAM.
Each step costs CPU time and RAM bandwidth. DirectStorage batches thousands of small reads into a queue
the NVMe drive processes in parallel, writes the compressed data to a GPU-visible buffer and runs a
GDeflate compute shader to decompress straight into the destination resource.

Mapping to this engine: the texture streamer keeps source mips in RAM (standing in for the disk) and
uploads synchronously with a per-frame budget (`textures.uploadBufferMB`). In a DirectStorage version:
- the streamer would enqueue `DSTORAGE_REQUEST`s for the needed mips of compressed `.ktx2/.dds` files;
- the per-frame budget becomes the *staging buffer size* given to the DirectStorage queue;
- completion fences tell the streamer when to swap the texture's view.

## Optimization
- Big wins: load times (2–5×), CPU usage during streaming; requires NVMe + D3D12 + a GPU decompression
  capable driver; HDD/SATA see much smaller gains.
- GPU decompression consumes GPU time (usually < 0.5 ms per frame while streaming).

## Rebuild
1. Pack assets with GDeflate compression per mip/tile.
2. Create a DirectStorage factory/queue (`DSTORAGE_QUEUE_DESC` with source FILE, destination GPU).
3. Enqueue requests (file, offset, size, destination resource/subresource), submit, signal a fence.
4. Swap resident mips when the fence completes; reuse the staging buffer ring.
