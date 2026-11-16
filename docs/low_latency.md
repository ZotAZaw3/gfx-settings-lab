# Low Latency Mode (Reflex / Anti-Lag style) and Max Frames in Flight

> Menu: **Latency → Low Latency Mode** (Off / On / On + Boost), **Max Frames in Flight** (1–4)
> Setting ids: `latency.mode`, `latency.maxFramesInFlight` · Owner: core
> Related: [vsync.md](vsync.md), [frame_rate_limit.md](frame_rate_limit.md), [latency_sync.md](latency_sync.md)

## Definition
Input latency is the time from moving the mouse to seeing the result. When the GPU is the bottleneck,
the CPU runs ahead and queues several frames; each queued frame was built from **old** input, so the
newest picture on screen is several frames behind your hand. Low latency modes (NVIDIA Reflex, AMD
Anti-Lag, Intel XeLL) stop that queue from forming: the CPU waits, reads input as late as possible,
and submits just in time for the GPU. You get the same FPS (sometimes 1–3 % less) with much lower lag.

## How it works
- **Max Frames in Flight (render-ahead queue)**: after each present the engine inserts a GPU fence. Before
  starting frame N it waits for the fence of frame N − max to signal. With max = 3, up to three frames of
  old input can be queued; with 1 the CPU never runs more than one frame ahead.
- **Low Latency On**: forces the queue depth to 1 and moves the wait **before** input sampling: the CPU
  sleeps first, then reads the mouse, then simulates and renders. That's the core idea of Reflex's
  "just-in-time" submission.
- **On + Boost**: additionally drains the GPU (`waitIdle`) every frame, so the CPU always starts on an
  idle GPU. This minimizes latency in CPU-bound and GPU-bound cases alike, but loses CPU/GPU overlap,
  costing throughput (Reflex Boost also keeps GPU clocks high, which we can't control from an app).
- Real Reflex additionally measures GPU render time and sleeps the CPU by exactly the amount that makes
  submission finish when the GPU becomes free (`NvAPI_D3D_Sleep` + latency markers); our version uses the
  fence wait as a simpler approximation.

## Breakdown
1. `Engine::frame()` start:
   - `device.setMaxFrameLatency(mode > 0 ? 1 : maxFramesInFlight)`
   - `device.waitForFrameLatency()` → GL: `glClientWaitSync` on the fence of frame N − depth
   - frame cap wait; Boost: `device.waitIdle()` (`glFinish`)
   - **then** `glfwPollEvents()` and camera update.
2. `swapBuffers()` inserts the next fence.
3. Overlay "wait ms" shows how long the CPU slept; latency ≈ CPU frame time + GPU frame time + scanout
   when the queue is 1 deep, versus + (depth × frame time) without it.

## Optimization
- When GPU-bound with V-Sync off: queue depth 3 → ~3 frames of latency; depth 1 → ~1 frame. At 60 fps
  that is ~50 ms → ~17 ms of render latency.
- Throughput cost: a depth of 1 removes the buffer that absorbs CPU spikes, so the frame-time graph can
  get spikier on uneven CPU loads. Boost costs more (no CPU/GPU overlap): visible as lower FPS in the
  bench table.
- Combine with a frame cap below refresh and VRR for the lowest latency without tearing.

<!-- bench: latency.mode latency.maxFramesInFlight -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Low Latency Mode | Off | -2.864 | +0.109 | +0.450 | +0.0 | +0.0 |
| Low Latency Mode | On | -0.757 | -1.614 | -1.566 | +0.0 | +0.0 |
| Low Latency Mode | On + Boost | -2.939 | -2.269 | -2.109 | +0.0 | +0.0 |
| Max Frames in Flight | 1 | -0.054 | +2.381 | +2.297 | +0.0 | +0.0 |
| Max Frames in Flight | 2 | +8.269 | +6.189 | +5.914 | +0.0 | +0.0 |
| Max Frames in Flight | 3 | +0.193 | +2.710 | +2.391 | +0.0 | +0.0 |
| Max Frames in Flight | 4 | +3.454 | +2.248 | +1.957 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild
1. After every present, insert a fence (GL `glFenceSync`, D3D11 query event, D3D12/Vulkan fence value).
2. At the top of the frame, wait on the fence from `N − depth` frames ago (D3D11/DXGI also offers
   `IDXGIDevice1::SetMaximumFrameLatency` + the waitable swapchain object).
3. Move input polling after that wait.
4. Optional "boost": wait for the GPU to be fully idle before starting the frame.
5. Verify with a latency tester (NVIDIA LDAT / high-speed camera), or the in-app proxy: the time between
   reading input and the present of that frame, which should drop by about (depth − 1) frames.

Files: `src/core/engine.cpp` (`frame`), `src/rhi/gl/gl_device.cpp` (`waitForFrameLatency`, `swapBuffers`)
