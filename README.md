# gfx-settings-lab

One prototype app that implements **every graphics setting a modern game exposes** — texture quality,
streaming, anti-aliasing, upscaling, frame generation, shadows, AO, reflections, ray/path tracing,
tessellation, LOD, particles, post-processing, V-Sync, low-latency, HDR and more — inside a single
OpenGL 4.5 / Direct3D 11 renderer with a game-style settings menu.

Every setting has:
- a doc in `docs/<setting>.md`: **Definition, How it works, Breakdown, Optimization, Rebuild**;
- a live **measured cost** in the menu (Δ GPU ms, Δ CPU ms, Δ VRAM, Δ RAM when you change it);
- an entry in the automatic benchmark (`gfxlab --bench-settings docs/_measurements`).

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gfxlab
```
Linux needs X11 dev packages (`libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev`).
Headless: `xvfb-run -a ./build/gfxlab --frames 60 --screenshot out.png`.

Keys: F1 settings menu · F2 overlay · F5 reload shaders · F12 screenshot · hold RMB + WASD/QE to fly, Shift = fast.

## Layout
See [docs/MODULE_API.md](docs/MODULE_API.md) for the architecture, the module contract and who owns what.
