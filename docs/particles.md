# Particles (GPU Particle Systems)

> Menu: **Effects → Particle Quality / Spawn Rate / Particle Lighting / Soft Particles / Particle Collision**
> Setting ids: `effects.particleQuality` (16k / 48k / 128k / 512k), `effects.spawnScale`, `effects.particleLighting`, `effects.softParticles`, `effects.particleCollision` · Owner module: `geometry.particles`
> Presets: quality Low…Ultra; lighting Unlit, Unlit, Lit, Lit; soft off, on, on, on; collision off, off, on, on
> Files: `src/modules/geometry/particles.cpp`, `shaders/geometry/particles_sim.comp`, `particles.{vert,frag}`

## Definition

Particles are large numbers of tiny, short-lived sprites (or mini meshes) used for fire,
smoke, sparks, rain, snow, dust, magic, blood and debris. The particle setting controls
**how many** are spawned, **how they are simulated** (CPU or GPU, with or without collision)
and **how expensive each one is to draw** (lighting, soft edges, texture resolution).

## How it works

A particle system is an emitter + a simulation + a renderer.

**Emitter.** Spawns N particles per second with randomized position, velocity, size,
colour, lifetime.

**Simulation (GPU path, used here).** All particles live in a Shader Storage Buffer:
```
struct Particle { vec4 posLife; vec4 velAge; vec4 color; };
```
A compute shader runs one thread per particle each frame:
1. `age += dt`; if `age > life` → respawn from the emitter (a ring buffer, or a
   dead-list with an atomic counter for exact spawn rates).
2. Integrate forces: gravity, drag, curl-noise turbulence, attractors.
   `vel += (gravity + noise(pos)*turbulence - vel*drag) * dt; pos += vel*dt;`
3. **Collision** (optional): reproject `pos` into the depth buffer; if behind the stored
   depth, the particle hit something → reflect velocity off the reconstructed normal.
   This "screen-space collision" is cheap and handles any geometry on screen.

**Rendering.**
- **Billboards**: the vertex shader expands each particle into a camera-facing quad using
  `gl_VertexID` (6 vertices per particle, no vertex buffer). Draw with
  `glDrawArrays(GL_TRIANGLES, 0, count*6)` and read the SSBO in the VS.
- **Blending**: additive (`GL_ONE, GL_ONE`) for fire/sparks needs no sorting; alpha-blended
  smoke needs back-to-front sorting (GPU bitonic sort) or OIT.
- **Soft particles**: fade alpha where the particle is close to scene depth
  `alpha *= saturate((sceneDepth - particleDepth) * softness)`. Removes the hard line where
  a sprite intersects the ground.
- **Lighting**: unlit (emissive), or per-vertex sun + ambient, or full per-pixel lighting
  with normal-mapped sprites; also receiving shadows.

## Breakdown

**In this prototype:**

1. Stage `FrameSetup`, order 50 — "Particle sim": 3 dispatches (sparks 30 %, smoke 30 %, fire 40 % of the budget),
   256 threads per group, SSBO binding 8. Collision reads **last frame's** `scene.depth` (the G-buffer hasn't run yet).
2. Stage `Translucent`, order 20 — "Particles": downsample depth if effects run below full res, draw
   `count × 6` vertices per emitter (no vertex buffer) into `geometry.fx` (RGBA16F, premultiplied), manual depth
   test + soft fade in the pixel shader, then composite onto `scene.color` with (ONE, ONE_MINUS_SRC_ALPHA).
3. Smoke is not sorted (premultiplied blending hides most errors at this density).


| Level  | Max particles | Sim       | Soft | Lighting | Collision |
|--------|---------------|-----------|------|----------|-----------|
| Low    | 16 k          | GPU       | off  | unlit    | off       |
| Medium | 64 k          | GPU       | on   | unlit    | off       |
| High   | 256 k         | GPU       | on   | lit      | on        |
| Ultra  | 1 M           | GPU       | on   | lit      | on        |

Costs:
- **Overdraw / fill rate** is the dominant cost: 50 layers of smoke over a 4K screen means
  each pixel is blended 50 times. Big, close-up particles are the worst.
- Simulation: cheap on the GPU (1 M particles ≈ well under a millisecond on a modern card).
- Sorting: O(n log² n) for bitonic, notable above ~100 k.
- Bandwidth: alpha blending reads and writes the colour target per layer.

## Optimization

- Render particles into a **half- or quarter-resolution buffer** and upsample with a
  depth-aware (bilateral) filter. Huge saving on smoke.
- Use **additive blending** where possible (no sort).
- Trim quads to the sprite's visible shape (octagon instead of quad) to cut overdraw ~30 %.
- Cap screen coverage: shrink or fade particles that get too close to the camera.
- Use atlases + flipbooks with motion-vector blending instead of high frame counts.
- LOD the emitters: halve spawn rate with distance; skip simulation when off-screen
  (bounding box test).
- Indirect draw: compute writes alive count into a `DrawArraysIndirectCommand`.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers.

<!-- bench: effects.particleQuality effects.spawnScale effects.particleLighting effects.softParticles effects.particleCollision -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Particle Quality | Low | -186.716 | -174.052 | -173.995 | -5.2 | +0.0 |
| Particle Quality | Medium | -129.425 | -103.135 | -103.134 | -3.8 | +0.0 |
| Particle Quality | High | -2.393 | +20.421 | +20.413 | +0.0 | +0.0 |
| Particle Quality | Ultra | +463.702 | +487.633 | +487.649 | +18.0 | +35.8 |
| Spawn Rate | 0.10 | -197.565 | -188.267 | -188.279 | +0.0 | +0.0 |
| Spawn Rate | 1.00 | -20.415 | +10.563 | +10.570 | +0.0 | +0.0 |
| Spawn Rate | 1.00 | -0.779 | +6.483 | +6.480 | +0.0 | +0.0 |
| Particle Lighting | Unlit | +9.575 | +7.260 | +7.256 | +0.0 | +0.0 |
| Particle Lighting | Lit | +20.655 | +3.504 | +3.514 | +0.0 | +0.0 |
| Soft Particles | Off | -1.998 | +7.714 | +7.713 | +0.0 | +0.0 |
| Soft Particles | On | +22.034 | +29.173 | +29.173 | +0.0 | +0.0 |
| Particle Collision | Off | -23.556 | -20.600 | -20.611 | +0.0 | +0.0 |
| Particle Collision | On | -13.839 | -14.927 | -14.909 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild (from scratch)

```glsl
// particles_sim.comp
layout(local_size_x = 256) in;
struct P { vec4 posLife; vec4 velAge; vec4 color; };
layout(std430, binding = 0) buffer Ps { P p[]; };
uniform float uDt, uTime; uniform vec3 uEmitter; uniform uint uCount;
float hash(uint n){ n=(n<<13U)^n; n=n*(n*n*15731U+789221U)+1376312589U; return float(n&0x7fffffffU)/float(0x7fffffff); }
void main(){
    uint i = gl_GlobalInvocationID.x; if (i >= uCount) return;
    P q = p[i];
    q.velAge.w += uDt;
    if (q.velAge.w > q.posLife.w) {               // respawn
        uint s = i*1973U + uint(uTime*1000.0)*9277U;
        q.posLife = vec4(uEmitter + (vec3(hash(s),hash(s+1U),hash(s+2U))-0.5)*0.5, 1.0+2.0*hash(s+3U));
        q.velAge  = vec4((hash(s+4U)-0.5)*2.0, 4.0+3.0*hash(s+5U), (hash(s+6U)-0.5)*2.0, 0.0);
    }
    q.velAge.xyz += (vec3(0,-3.0,0) - q.velAge.xyz*0.3) * uDt;
    q.posLife.xyz += q.velAge.xyz * uDt;
    p[i] = q;
}
```
```cpp
glUseProgram(simProg); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
glDispatchCompute((count + 255) / 256, 1, 1);
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
glUseProgram(drawProg); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
glDepthMask(GL_FALSE); glDrawArrays(GL_TRIANGLES, 0, count * 6);
```
The vertex shader reads `p[gl_VertexID / 6]`, picks a corner from `gl_VertexID % 6`, and
offsets it along the camera's right/up vectors.
