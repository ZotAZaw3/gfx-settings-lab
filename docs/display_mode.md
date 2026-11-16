# Window Mode and Resolution

> Menu: **Display → Display** · Setting ids: `display.windowMode` (Windowed / Borderless / Fullscreen), `display.resolution` · Owner: core

## Definition
- **Windowed**: the game is a normal window on the desktop at the chosen resolution.
- **Borderless (windowed fullscreen)**: a window without decorations covering the whole screen at the
  desktop resolution. Alt-Tab is instant and overlays work; resolution is always native.
- **Exclusive fullscreen**: the game takes over the display and can change its resolution and refresh
  rate. Historically lower latency and required for some VRR/HDR paths.
- **Resolution**: the output resolution (the size of the final image). Changing it changes UI size and
  sharpness; the 3D cost can be adjusted independently with Render Resolution.

## How it works
- GLFW: `glfwSetWindowMonitor(window, nullptr, …)` for windowed/borderless (borderless = undecorated
  window sized to the monitor's video mode), `glfwSetWindowMonitor(window, monitor, …, refresh)` for
  exclusive. The framebuffer size callback triggers a swapchain + target resize.
- On Windows 10/11 with flip-model swapchains (DXGI_SWAP_EFFECT_FLIP_DISCARD), borderless windows get
  "independent flip": the compositor is bypassed when the game covers the screen, so latency matches
  exclusive fullscreen and VRR works. That's why modern games default to borderless.
- Exclusive fullscreen still matters when you want a non-native resolution scaled by the display, or a
  different refresh rate.

## Breakdown
1. Setting change → `Engine::applyWindowMode()` → GLFW monitor/window calls → `applyDisplaySettings()`
   (swap interval can reset on mode switches).
2. Next frame `handleResize()` sees the new framebuffer size → swapchain resize, `RenderTargets::resize`,
   every module's `resize()`.

## Optimization
- Output resolution scales the cost of every output-res pass (bloom, tone map, UI) and, through Render
  Resolution %, the 3D passes. 4K has 4× the pixels of 1080p.
- Windowed mode on Windows uses the compositor (DWM): +1 frame of latency unless the swapchain is
  flip-model and the window qualifies for independent flip.
- Mode switches are slow (hundreds of ms) and reallocate everything; never do them per frame.

<!-- bench: display.windowMode display.resolution -->
<!-- /bench -->
(The benchmark skips these: they need a real monitor. Measure output-resolution cost with Render Resolution instead.)

## Rebuild
1. Enumerate monitors and video modes; store the user's choice.
2. Windowed: create/resize a decorated window. Borderless: undecorated window at the monitor rect.
   Exclusive: platform fullscreen at the chosen mode and refresh.
3. Recreate swapchain buffers on size change (D3D11: `ResizeBuffers`; GL: implicit) and every
   size-dependent target.
4. Re-apply vsync/HDR after switching.

Files: `src/core/engine.cpp` (`applyWindowMode`, `handleResize`)
