# Latency and Sync: Reflex, Anti-Lag, V-Sync, VRR (G-Sync / FreeSync), Frame Cap

> Menu: **Latency (core)** · Setting id(s): core settings · Owner module: `core`
> Values: see below · Presets: core

Settings shown in the menu: `V-Sync: Off | On | Adaptive`, `Low Latency: Off | On (ours) | Reflex* | Anti-Lag 2*`, `Frame Cap`, `Triple Buffering`, `Max Frames in Flight`, `VRR info` (read-only)

## Definition

- **System latency** = time from mouse click to the photon on screen: input sampling → CPU simulation → render submission → GPU queue wait → GPU render → present queue → scanout.
- **V-Sync** makes the swap wait for the display's vertical blank, so a frame is never shown half old/half new (tearing). Costs latency and stutters when fps falls below the refresh rate (60 → 30).
- **Adaptive V-Sync** (`swap interval -1`, "late swap tearing"): V-Sync when above refresh rate, tear instead of stalling below it.
- **VRR (G-Sync, FreeSync, HDMI VRR):** the display waits for the GPU instead of the GPU waiting for the display. No tearing and no stutter as long as fps is inside the VRR range. Best used with V-Sync on in the driver and a frame cap a few fps below the max refresh.
- **NVIDIA Reflex / AMD Anti-Lag 2 / Intel XeLL:** keep the GPU's render queue empty by delaying the CPU's frame start until just in time, so input is sampled as late as possible. Typically cuts latency 20-50% when GPU-bound.

## How it works

When GPU-bound, the CPU runs ahead and queues 2-3 frames; each frame then sits in the queue waiting for the GPU, and its input is stale by that much. Fixes:

1. **Max frames in flight = 1:** wait on a fence for frame N-1 before starting simulation of frame N+1 (OpenGL: `glFenceSync` + `glClientWaitSync`; D3D11: `IDXGIDevice1::SetMaximumFrameLatency(1)` or a waitable swap chain).
2. **Just-in-time start (Reflex-like):** measure GPU frame time `G` and CPU frame time `C`; sleep at the start of the frame for `max(0, G - C - margin)` so the CPU finishes submission exactly when the GPU becomes free. Input is sampled after the sleep.
3. **Frame cap:** a precise sleep+spin limiter. Capping below the GPU limit removes the queue entirely (and below VRR max keeps VRR active).
4. **Markers:** Reflex SDK takes markers (simulation start/end, render submit, present) so the driver can do step 2 precisely and measure PC latency.

## Breakdown

| Mode | Tearing | Latency (GPU-bound, 60 fps) | Stutter below refresh |
|---|---|---|---|
| V-Sync off | yes | lowest (~1 frame) | no |
| V-Sync on (double buffer) | no | +1-2 frames | yes (halves fps) |
| V-Sync + triple buffer | no | +1-2 frames | less |
| Adaptive V-Sync | below refresh only | low | no |
| VRR + cap | no | low | no |
| + Low latency (ours/Reflex) | - | -20 to -50% | - |

Code: the core implements these settings (`src/modules/core/`): (frames-in-flight fences, JIT sleep controller, frame limiter, latency overlay: estimated input-to-present time using timestamps). V-Sync is applied through the core's `rhi::setSwapInterval` (GLFW `glfwSwapInterval(0/1/-1)`, D3D `Present(syncInterval)` + `DXGI_PRESENT_ALLOW_TEARING`). Reflex/Anti-Lag 2 SDK hooks need D3D11/12 and their SDKs; the OpenGL path uses our implementation.

## Optimization

- Sample input after the JIT sleep, as late as possible.
- Use `DXGI_SWAP_EFFECT_FLIP_DISCARD` + waitable object on D3D (flip model, lower latency, enables VRR).
- Frame limiter: sleep to 1-2 ms before the target, then spin; Windows: `timeBeginPeriod(1)` or high-resolution waitable timer.
- With VRR, cap at `refresh - 3` fps (e.g. 141 on 144 Hz).
- Frame generation adds latency, so force low-latency on when FG is enabled.

## Rebuild (from scratch)

```cpp
// frame start
float sleepMs = std::max(0.f, gpuMsEma - cpuMsEma - 0.5f);   // JIT
preciseSleep(sleepMs);
input.poll();                                    // latest possible input
simulate(); render();                            // record cpuMs
fences[frame % N] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
present();
glClientWaitSync(fences[(frame + 1) % N], GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX); // N = frames in flight
```

Steps: add GPU timer + CPU timer, fence ring with configurable depth, JIT sleep, frame limiter, and an overlay graph of CPU, GPU, queue and estimated latency. Compare: V-Sync on/off, frames in flight 3 vs 1, JIT on/off.
