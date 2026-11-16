# Field of View (FOV)

> Menu: **Display → Camera → Field of View** · Setting id: `display.fov` (40–120°, vertical, default 70) · Owner: core

## Definition
FOV is how wide the camera sees. A higher FOV shows more of the world at the edges (useful in fast
shooters and on ultrawide screens) but makes objects in the center smaller and stretches the edges; a
lower FOV feels zoomed in. Sitting far from a TV usually calls for lower FOV, close to a monitor higher.

## How it works
The projection matrix maps view-space positions to the screen: `x_ndc = x / (z · tan(fov/2) · aspect)`.
This engine uses **vertical** FOV (Hor+ scaling): widening the window adds horizontal view instead of
cropping. Horizontal FOV is `2·atan(tan(vfov/2) · aspect)`; 70° vertical at 16:9 ≈ 102° horizontal.
The projection is infinite reversed-Z (`P[3][2] = near`, depth = near / viewDistance), so FOV doesn't
affect depth precision.

## Breakdown
1. `Engine::frame()`: `camera.fovY = radians(display.fov)`.
2. `fillFrameConstants()`: `Camera::projection()` → `FrameConstants.proj/viewProj` (+ jitter for TAA).
3. Everything downstream (culling, LOD, texture streaming estimates, screen-space effects) reads it.

## Optimization
- Wider FOV → more objects in the frustum → more draw calls/triangles (CPU and vertex cost), and objects
  appear smaller so LOD/streaming pick lower detail. Pixel cost stays about the same.
- Screen-space effects (SSR, SSAO radius in pixels) behave differently at the edges of very wide FOV;
  > 110° shows strong rectilinear stretching. Panini projection (post module) can reduce it.

<!-- bench: display.fov -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Field of View | 40 deg | +14.013 | +18.256 | +18.247 | +1.0 | +1.4 |
| Field of View | 70 deg | +0.205 | +3.903 | +3.699 | +0.0 | +1.4 |
| Field of View | 120 deg | -17.691 | -15.057 | -15.227 | -0.6 | +1.4 |
<!-- /bench -->
(On a software rasterizer the change is dominated by how much of the screen the textured ground covers,
so the deltas are larger than they'd be on a GPU.)

## Rebuild
1. Store vertical FOV; build `P` with `f = 1/tan(fov/2)`: `P[0][0] = f/aspect, P[1][1] = f`.
2. Convert user-facing horizontal FOV (if you display it) with `vfov = 2·atan(tan(hfov/2)/aspect)`.
3. Re-derive frustum planes for culling and the screen-size LOD metric from the same matrix.

Files: `src/core/renderer.cpp` (`Camera::projection`), `src/core/engine.cpp`
