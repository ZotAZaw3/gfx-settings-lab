# Shader Cache and Shader Precompilation

> Menu: **Graphics → Shaders → Shader Cache** · Setting id: `shaders.cache` · Owner: core
> Related: [shader_hot_reload.md](shader_hot_reload.md)

## Definition
Before the GPU can run a shader, the driver compiles it from source (or from an intermediate format like
DXIL/SPIR-V) into the GPU's own machine code. That can take tens of milliseconds per shader. If it
happens in the middle of gameplay the frame freezes: the infamous **shader compilation stutter**. Two
fixes:
- **Precompilation**: compile every shader the game will need during the loading screen / first launch
  ("Compiling shaders… 43 %").
- **Shader cache**: save the compiled machine code to disk and reuse it next time, so the second launch is
  fast. Drivers also keep their own caches (NVIDIA's "Shader Cache Size" control panel option).

## How it works
- This engine **precompiles**: every module creates its pipelines in `init()`, so all programs are
  compiled before the first frame (nothing compiles mid-game unless you edit a shader with hot reload or
  change a setting flagged `RequiresReload`).
- The **cache** stores GL program binaries: on build, the backend hashes the preprocessed sources of all
  stages + the GPU/driver strings; if `shader_cache/<hash>.bin` exists, it calls `glProgramBinary` (no
  compile, no link); otherwise it compiles, links, then saves `glGetProgramBinary` output (requested with
  `GL_PROGRAM_BINARY_RETRIEVABLE_HINT`). A driver update changes the key or makes `glProgramBinary` fail,
  and we fall back to compiling. D3D11 caches the `D3DCompile` bytecode the same way (keyed on the HLSL).
- Toggling the setting reloads all shaders (`SettingRequiresReload`) so you can see the difference live in
  the counters `shaders.compileMsTotal`, `shaders.cacheHits`, `shaders.cacheMisses`.

Measured in the cloud (llvmpipe), 14 core programs: **220 ms compile cold → 13 ms from cache** (17×).
Real GPU drivers on large games: minutes → seconds.

## Breakdown
1. `Engine::init`: `device.setProgramCacheDir(shaders.cache ? "shader_cache" : "")` before any program.
2. `ShaderLibrary::build` → `Device::rebuildProgram` → hash → `glProgramBinary` or compile + store.
3. Overlay counters show cumulative compile ms and hits/misses.

## Optimization
- The cache costs disk (a few KB–MB per program) and nothing per frame.
- Keep the permutation count low: every define combination is another binary.
- On D3D12/Vulkan, the equivalent is the **pipeline state object** cache (`ID3D12PipelineLibrary`,
  `VkPipelineCache`), and the hard part is knowing every PSO combination ahead of time: games record the
  PSOs seen during play ("PSO gathering" in Unreal) and precompile them at load.
- Asynchronous compilation (compile on worker threads, draw with a simpler fallback until ready) hides the
  stutter at the cost of brief visual differences.

<!-- bench: shaders.cache -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits_.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Shader Cache | Off | +4.736 | +6.225 | +5.831 | +0.0 | +3.9 |
| Shader Cache | On | +2.356 | +0.468 | -0.066 | +0.0 | +5.7 |
<!-- /bench -->

## Rebuild
1. Compile all shaders at load (or a gathered list of permutations).
2. Key = hash(sources + defines + driver version + GPU); value = driver binary.
3. Load binary first; on failure compile and store.
4. Invalidate on driver change; cap the cache size; store in a user-writable folder.

Files: `src/rhi/gl/gl_device.cpp` (`rebuildProgramImpl`), `src/core/core_passes.cpp`
