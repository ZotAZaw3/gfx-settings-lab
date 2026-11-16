# Performance Overlay, Debug Views and Per-Setting Cost (CPU / GPU / RAM / VRAM)

> Menu: **Debug → Overlay → Performance Overlay** (Off / FPS / Detailed), **Debug → Buffers → Debug View**
> Setting ids: `debug.overlay`, `debug.view` · Keys: F2 toggles the overlay · Owner: core (`Profiler`, `CoreGui`)

## Definition
The overlay shows what each frame costs and where the time and memory go: FPS and frame time, 1 % low,
CPU and GPU milliseconds, CPU usage and GPU busy %, RAM and VRAM, render vs output resolution, a
frame-time graph, a per-pass table (GPU and CPU ms for every render pass, owned by which module), memory
per module and custom counters. Every setting in the menu also shows **its own measured cost**: when
you change it, the profiler compares 60 frames before with 60 frames after (skipping 15 warm-up
frames) and prints Δ GPU ms, Δ CPU ms, Δ VRAM and Δ RAM next to the control.

Debug View replaces the final image with an internal buffer: albedo, normals, roughness, metallic,
material AO, depth, motion vectors, emissive, or the HDR scene before post-processing.

## How it works
- **GPU time per pass**: two timestamp queries around each pass (`glQueryCounter(GL_TIMESTAMP)` /
  D3D11 timestamp + disjoint queries). Results are read from a ring of 6 query pairs, i.e. ~5 frames
  later, so the CPU never stalls waiting for the GPU. Values are exponentially smoothed (α = 0.1).
- **CPU time per pass**: `high_resolution_clock` around the pass callback (command recording cost).
- **Frame**: CPU ms = frame time minus waits (latency limiter, frame cap); GPU ms = sum of pass times.
  "GPU busy" = GPU ms / frame ms.
- **RAM**: process resident set (`/proc/self/statm`, `GetProcessMemoryInfo`).
- **VRAM**: every `rhi` texture/buffer reports its size and `owner` tag at creation, so totals are exact
  for what the app allocated (driver overhead excluded). When available, the driver's own numbers
  (`GL_NVX_gpu_memory_info`, DXGI `QueryVideoMemoryInfo`) are shown too.
- **CPU usage**: process CPU time / wall time (100 % = one core).
- **Setting cost probe**: `SettingsRegistry::notifyChanged` → `Profiler::onSettingChanged` stores the
  60-frame average before the change, then collects 60 frames after 15 warm-up frames and stores the
  delta. Sliders commit on release so a drag counts as one change.
- **Benchmark sweep**: `gfxlab --bench-settings <dir>` sets the High preset, disables V-Sync and the
  frame cap, and for every setting value measures warm-up + 60 frames vs a fresh baseline, then writes
  `<dir>/<gpu>.md` and `.csv`. `tools/fill_bench_tables.py` pastes the rows into each doc's
  `<!-- bench: ids -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
<!-- /bench -->

## Rebuild
1. Allocate a ring of timestamp query pairs per pass; write begin/end around the pass; read the oldest slot
   before reusing it.
2. Track allocations per owner in your RHI (`create*` adds, destructor subtracts).
3. Keep a rolling history of frame samples; compute averages and percentiles from it.
4. For per-setting cost: snapshot the average before a change, discard warm-up frames after, average a
   window, subtract.
5. Display with ImGui; never block on GPU readbacks.

Files: `src/core/profiler.cpp`, `src/core/gui.cpp`, `src/core/engine.cpp` (`runStage`, `runBenchmarkSweep`)
