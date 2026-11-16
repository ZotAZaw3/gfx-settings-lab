# Particles (GPU Particle Systems)

Menu: **Effects → Particle Quality** (`Low | Medium | High | Ultra`),
**Particle Lighting** (`Unlit | Lit`), **Soft Particles** (on/off), **Particle Collision** (on/off)
Code: `GeoVfxSettings::particleQuality`, `particleLit`, `softParticles`, `particleCollision`;
shaders `particles_sim.comp`, `particles.vert/.frag`

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
