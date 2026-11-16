# Shaders: Hot Reload, Includes, Permutations and Compilation

> Menu: **Debug → Shaders → Shader Hot Reload** · Setting id: `shaders.hotReload` · F5 = reload all · Owner: core (`ShaderLibrary`)
> Related: [shader_cache_precompilation.md](shader_cache_precompilation.md)

## Definition
Shaders are the small programs the GPU runs for every vertex and pixel. Hot reload watches the shader
files and recompiles any program whose source (or any file it includes) changed, while the app keeps
running. It's a development feature: edit `shaders/lighting/ssao.frag`, save, see the result next frame.

## How it works
- `ShaderLibrary::get(desc)` keys programs by stage files + defines and returns a stable `Program*`.
- **Preprocessing**: our own `#include "path"` resolver (relative to `shaders/`), with `#line` markers so
  errors point at the right file, then `#version 450 core`, backend define (`GFX_GL` / `GFX_D3D11`), global
  defines and per-program defines (the **permutation**: e.g. `ALPHA_TEST=1`, `FILTER=2`).
- Every file read is recorded as a dependency with its timestamp. With hot reload on, every 30 frames
  the library compares timestamps and rebuilds changed programs. A failed compile keeps the last good
  binary and shows the error in the overlay.
- The backend swaps the binary **inside** the same `Program` object (`Program::generation()` bumps), so
  pipelines and modules never hold dangling pointers.
- D3D11 path: the same GLSL is compiled to SPIR-V by glslang, translated to HLSL by SPIRV-Cross, and
  compiled with `D3DCompile`, so hot reload works there too.

## Breakdown
1. `Engine::frame()` → `shaders.checkForChanges()` (every 30 frames if enabled).
2. `ShaderLibrary::build()` → `preprocess()` per stage → `Device::rebuildProgram()` (GL: compile,
   link, delete old program on success).
3. Overlay: last shader error; the profiler records the compile time as CPU cost in the frame it happens.

## Optimization
- Polling cost: one `stat()` per dependency every 30 frames (tiny; see bench).
- Compile cost: 1–50 ms per program on GL drivers; a reload hitch is expected and only happens on edit.
- Shipping games turn this off and instead precompile/cache (see the precompilation doc): runtime
  compile hitches ("shader compilation stutter") are the #1 PC stutter complaint.
- Keep permutations few: each define combination is a separate program to compile and cache.

<!-- bench: shaders.hotReload -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Shader Hot Reload | Off | +2.936 | +2.941 | +3.147 | +0.0 | +0.0 |
| Shader Hot Reload | On | +0.813 | +1.661 | +1.734 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild
1. Write a preprocessor: resolve includes recursively, guard against cycles, emit `#line`, prepend version + defines.
2. Record dependency timestamps per program; poll or use OS file watchers (ReadDirectoryChangesW / inotify).
3. Rebuild into a temporary program; swap only on success; keep handles stable through an indirection.
4. Surface errors in-app; never crash on a bad edit.

Files: `src/core/shader_library.cpp`, `src/rhi/gl/gl_device.cpp` (`rebuildProgram`)
