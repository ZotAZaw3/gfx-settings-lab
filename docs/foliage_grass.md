# Grass and Foliage (Density, Quality, Draw Distance)

Menu: **World → Grass Density** (0 – 100 %), **Foliage Quality** (`Low | Medium | High | Ultra`),
**Foliage Draw Distance** (20 – 400 m), **Grass Interaction** (on/off), **Wind Animation** (on/off)
Code: `GeoVfxSettings::grassDensity`, `foliageQuality`, `foliageDistance`, `grassInteract`,
`foliageWind`; shaders `geometry/grass_gen.comp`, `geometry/grass.vert/.frag`

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
