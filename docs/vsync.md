# V-Sync

> Menu: **Display → Sync → V-Sync** · Setting id: `display.vsync` · Values: Off / On / Adaptive / Half rate · Owner: core
> Related: [frame_rate_limit.md](frame_rate_limit.md), [low_latency.md](low_latency.md), [latency_sync.md](latency_sync.md)

## Definition
Vertical sync makes the game wait for the monitor to finish drawing the current image before showing the
next one. Without it the new frame can replace the old one halfway down the screen, which shows up as a
horizontal **tear** during camera motion. With it there is no tearing, but frames can only change at the
refresh rate, a frame that misses a refresh waits for the next one (stutter: 60 → 30 fps steps), and
input latency rises by up to a frame or more.

## How it works
The swapchain has a front buffer (being scanned out) and one or more back buffers (being rendered).
"Present" swaps them.
- **Off** (`swap interval 0` / `SyncInterval 0` + tearing allowed): swap immediately. Lowest latency,
  tearing whenever the swap lands mid-scanout.
- **On** (`interval 1`): swap only at vertical blank. If the frame took 17 ms on a 60 Hz panel, it waits
  until the 33.3 ms vblank.
- **Adaptive** (`interval −1`, `EXT_swap_control_tear` / "Fast Sync"-like behaviour on drivers that
  support it): sync when the frame is on time, tear when it's late instead of dropping to half rate.
  Falls back to On when the extension is missing.
- **Half rate** (`interval 2`): sync to every second vblank, e.g. a steady 30 fps on 60 Hz.
- **VRR (G-Sync / FreeSync / HDMI VRR)**: the display waits for the GPU instead, so there's no tearing
  and no quantization inside the VRR range. It's enabled in the driver and needs exclusive fullscreen or
  a flip-model swapchain; the best setup is VRR + V-Sync On + a frame cap a few fps below the max refresh.

## Breakdown
1. Setting change → `Engine::applyDisplaySettings()` → `Device::setPresentMode()`.
2. GL backend: `glfwSwapInterval(0 | 1 | -1 | 2)`. D3D11 backend: `Present(syncInterval, flags)` with
   `DXGI_PRESENT_ALLOW_TEARING` for Off on a flip-model swapchain.
3. The wait shows up in the overlay as "wait" time / longer frame time, not GPU time.

## Optimization
- V-Sync doesn't cost GPU time; it costs **latency** and frame-time **quantization**.
- Triple buffering (3 back buffers) lets the GPU keep working while waiting for vblank, avoiding the
  60→30 drop, at one extra frame of latency. Our "Max Frames in Flight" controls the CPU side of that.
- For lowest latency without tearing: VRR + cap 3 fps under refresh + Low Latency Mode.

<!-- bench: display.vsync -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
<!-- /bench -->

(Benchmarks run with V-Sync forced Off so other settings are measured unthrottled; V-Sync itself is
judged by frame pacing in the overlay graph, not by a delta.)

## Rebuild
1. Create the swapchain with the flip model (DXGI) / a FIFO + MAILBOX/IMMEDIATE capable surface (Vulkan).
2. Map the setting: Off → IMMEDIATE (+tearing flag), On → FIFO, Adaptive → FIFO_RELAXED, Half → interval 2.
3. Re-apply after fullscreen/window changes (some drivers reset swap interval per context/swapchain).
4. Verify with a fast horizontal pan: tearing lines visible with Off, gone with On.

Files: `src/core/engine.cpp` (`applyDisplaySettings`), `src/rhi/gl/gl_device.cpp` (`setPresentMode`)
