# Grass and Foliage (Density, Quality, Draw Distance)

> Menu: **World → Grass Density / Foliage Quality / Foliage Draw Distance / Wind Animation / Grass Interaction**
> Setting ids: `world.grassDensity`, `world.foliageQuality`, `world.foliageDistance` (20–300 m), `world.foliageWind`, `world.grassInteraction` · Owner module: `geometry.foliage`
> Presets: density 0.25 / 0.5 / 0.75 / 1; quality Low…Ultra (3 / 5 / 7 / 15 vertices per blade); distance 40 / 80 / 120 / 250 m
> Files: `src/modules/geometry/foliage.cpp`, `shaders/geometry/grass_gen.comp`, `grass.{vert,frag}`

## Definition

Grass and foliage settings control how many grass blades, bushes, flowers and tree
leaves are drawn, how far away they keep being drawn, how detailed each plant is, and
whether they move in the wind or bend when the player walks through them. Vegetation is
often the single largest object count in an outdoor scene, so these settings have a big
fps effect in open-world games.

## How it works

1. **Placement.** A density map (painted by artists or derived from terrain material)
   says how much grass grows where. At runtime the terrain around the camera is split
   into tiles (e.g. 8 × 8 m). For each tile a compute shader scatters candidate points
   using a deterministic hash of the tile coordinate (so grass does not reshuffle), keeps
   a fraction equal to `density * densityMap(p)`, and appends `{position, rotation,
   height, type}` to an instance buffer.
2. **Density falloff with distance.** Instances get a random value `r ∈ [0,1)`; they are
   kept only if `r < density * falloff(dist)`. Far away fewer blades survive, and survivors
   are made **wider** to keep the same coverage (otherwise distant grass looks thin).
3. **Draw distance.** Beyond `foliageDistance` grass is not generated at all; the terrain
   texture provides a grass colour so the cut-off is less obvious (and a dithered fade
   hides the edge).
4. **Geometry.** Close blades: 5–15 vertices each, bent along a Bézier curve in the VS.
   Mid-distance: 3-vertex blades or grass "cards" (alpha-tested quads with many blades
   painted on). Trees: mesh LODs → billboards / octahedral impostors.
5. **Wind.** Vertex shader offsets the blade tip with scrolling noise; stiffness grows
   towards the root. Trees use a hierarchical sway (trunk, branch, leaf flutter).
6. **Interaction.** Characters write their position into a small "trample" texture
   around the camera; blades read it and bend away, recovering over time.

## Breakdown

**In this prototype:**

1. Stage `GBuffer`, order 15 — "Grass": 4 compute dispatches, one per distance tier (dist/8, /4, /2, 1), cell
   size doubling per tier and snapped to world space so blades don't swim; each thread jitters a blade, applies
   density falloff, rejects plaza/lake/snow/cliffs, frustum-culls and `atomicAdd`s into an indirect draw.
2. One `drawIndirect` of triangle strips writes the G-buffer with shading model 2 (foliage).
3. Grass does not cast shadows in this prototype (the lighting thread's shadow pass draws only the scene DrawList).


| Foliage quality | Blade verts | Max distance | Shadows from grass | Tree LOD bias |
|-----------------|-------------|--------------|--------------------|---------------|
| Low             | 3 (card)    | 40 m         | none               | +2            |
| Medium          | 5           | 80 m         | none (contact AO)  | +1            |
| High            | 7           | 150 m        | near cascade only  | 0             |
| Ultra           | 15          | 300 m        | all cascades       | 0             |

Costs:
- **Instances × vertices** (vertex-bound) for blade geometry.
- **Alpha-test overdraw** for cards and leaves: `discard` disables some early-Z
  optimisations; dense layers of leaves are expensive at high resolution.
- **Shadows**: grass casting into shadow maps can double its cost.
- CPU: near zero if generated on the GPU and drawn with one indirect call.

## Optimization

- Generate on the GPU per tile, cull tiles by frustum + Hi-Z, draw indirect.
- Prefer real geometry blades over alpha cards up close (no overdraw/discard cost).
- Depth pre-pass for alpha-tested foliage, then shade with `EQUAL` depth test.
- Widen blades with distance instead of keeping density.
- Skip grass shadow casting beyond the first cascade; use AO/contact shadows instead.
- Tie grass density to the upscaler: at low internal resolution, thin blades alias,
  so slightly wider blades look better.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers.

<!-- bench: world.grassDensity world.foliageQuality world.foliageDistance world.foliageWind world.grassInteraction -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Grass Density | 0.00 | +10.407 | +3.653 | +3.648 | +0.0 | +0.0 |
| Grass Density | 0.75 | +5.276 | +2.919 | +2.910 | +0.0 | +0.0 |
| Grass Density | 1.00 | +13.232 | +8.502 | +8.481 | +0.0 | +0.0 |
| Foliage Quality | Low | +2.623 | +4.974 | +4.946 | +0.0 | +0.0 |
| Foliage Quality | Medium | +5.594 | +9.025 | +9.013 | +0.0 | +0.0 |
| Foliage Quality | High | +6.932 | +10.637 | +10.648 | +0.0 | +0.0 |
| Foliage Quality | Ultra | +26.963 | +27.851 | +27.889 | +0.0 | +0.0 |
| Foliage Draw Distance | 20 m | -17.606 | -16.424 | -16.439 | +0.0 | +0.0 |
| Foliage Draw Distance | 120 m | -7.237 | -14.930 | -14.949 | +0.0 | +0.0 |
| Foliage Draw Distance | 300 m | -4.525 | -0.436 | -0.433 | +0.0 | +0.0 |
| Wind Animation | Off | +17.815 | +10.820 | +10.794 | +0.0 | +0.0 |
| Wind Animation | On | +19.035 | +16.734 | +16.706 | +0.0 | +0.0 |
| Grass Interaction | Off | +3.785 | +9.805 | +9.807 | +0.0 | +0.0 |
| Grass Interaction | On | -7.204 | -6.004 | -5.986 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild (from scratch)

```glsl
// grass_gen.comp — one thread per candidate in a tile around the camera
layout(local_size_x = 64) in;
struct Blade { vec4 posRot; vec4 params; };    // xyz,rotation | height,width,bend,type
layout(std430, binding=0) writeonly buffer Out { Blade blades[]; };
layout(binding=1) uniform atomic_uint uCount;  // also copied into the indirect cmd
uniform vec2 uTileOrigin; uniform float uTileSize, uDensity, uMaxDist; uniform vec3 uCam;
uniform sampler2D uHeight; uniform float uTerrainScale, uDisp;
float h1(vec2 p){ return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453); }
void main(){
    uint i = gl_GlobalInvocationID.x; vec2 cell = vec2(i % 128u, i / 128u);
    vec2 xz = uTileOrigin + (cell + vec2(h1(cell), h1(cell+7.0))) * (uTileSize/128.0);
    float dist = distance(xz, uCam.xz);
    float keep = uDensity * (1.0 - smoothstep(uMaxDist*0.5, uMaxDist, dist));
    if (h1(xz) >= keep) return;
    float y = textureLod(uHeight, xz * uTerrainScale, 0.0).r * uDisp;
    uint idx = atomicCounterIncrement(uCount);
    float widen = 1.0 + dist / uMaxDist * 2.0;
    blades[idx] = Blade(vec4(xz.x, y, xz.y, h1(xz+3.1)*6.283),
                        vec4(0.4 + 0.4*h1(xz+1.7), 0.03*widen, h1(xz+5.3), 0.0));
}
```
Then `glDrawArraysInstancedIndirect`-style: one blade = triangle strip of N vertices;
the VS computes the vertex from `gl_VertexID` along the blade and `gl_InstanceID`
picks the blade.
