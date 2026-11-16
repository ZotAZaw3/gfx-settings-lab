# Frame Rate Limit (Frame Cap)

> Menu: **Display → Sync → Frame Rate Limit** · Setting id: `display.frameCap` (0 = unlimited, up to 360) · Owner: core

## Definition
A frame cap stops the game from rendering faster than a chosen rate. It gives steadier frame pacing
(every frame takes the same time), lowers GPU power, heat and fan noise, and, when set just below a VRR
monitor's maximum refresh, keeps the game inside the VRR range with no V-Sync lag.

## How it works
At the start of each frame the engine computes the time the frame is *allowed* to start:
`target = lastFrameStart + 1 / cap`. It sleeps until ~1.5 ms before the target (OS sleep is coarse) and
spins the rest, then samples input and renders. Because the wait happens **before** input is read
(not after rendering), the cap adds no extra input latency: that's the difference between a good
in-engine limiter and a driver/external limiter that blocks in Present.

If a frame runs late the next target is based on the real start time (`max(target, now − 0.25 s)`) so the
limiter never tries to "catch up" with a burst of fast frames.

## Breakdown
1. `Engine::frame()` start: `device.waitForFrameLatency()` (queue depth) → frame-cap wait
   (`preciseWaitUntil`) → measured as `wait ms` in the overlay.
2. `glfwPollEvents()` + camera input happen after the wait (just-in-time input).

## Optimization
- Costs nothing on the GPU; it *removes* work. CPU usage drops as well (sleeping instead of spinning,
  except for the final ~1.5 ms spin, which trades a little CPU for pacing precision).
- The best cap for VRR is `refresh − 3` (e.g. 141 fps at 144 Hz).
- The cap is only as precise as the OS timer: Windows `timeBeginPeriod(1)` or high-resolution waitable
  timers help; Linux nanosleep is usually fine.

<!-- bench: display.frameCap -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
<!-- /bench -->

## Rebuild
1. Keep `lastFrameStart` (double seconds, monotonic clock).
2. At frame start: `target = lastFrameStart + 1/cap; sleep(target - now - 1.5ms); spin until target`.
3. `lastFrameStart = max(target, now - 0.25)`; then poll input and build the frame.
4. Verify: the overlay frame-time graph becomes a flat line at `1000/cap` ms.

Files: `src/core/engine.cpp` (`frame`, `preciseWaitUntil`)
