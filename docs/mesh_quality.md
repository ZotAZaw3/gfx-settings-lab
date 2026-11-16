# Model / Mesh Quality

> Menu: **Geometry → Model Quality** · Setting id: `geometry.meshQuality` · Owner module: `geometry.lod`
> Values: Low / Medium / High / Ultra · Presets: Low=Low, Medium=Medium, High=High, Ultra=Ultra
> Files: `src/modules/geometry/lod_culling.cpp`, `src/modules/geometry/geo_util.cpp` (`simplifyClustered`)

## Definition

Model quality decides how many triangles (and how much per-vertex data) each 3D model is
drawn with. It is the "base" geometric detail of the scene before LOD, tessellation or
displacement add or remove anything. In most games it also controls the quality of
skinning (bone influences per vertex), morph targets, hair/fur strand counts and foliage
density, because those all scale with vertex count.

## How it works

1. **Authoring.** An artist builds a high-poly sculpt (millions of triangles) and a
   game-ready mesh (thousands). The detail lost between the two is baked into a
   normal map so the low mesh still *looks* detailed under lighting.
2. **Offline simplification.** A tool (meshoptimizer, Simplygon, Unreal's reduction)
   produces a chain of meshes at e.g. 100 %, 50 %, 25 %, 12.5 % of the triangle count
   using *quadric error metrics*: each candidate edge collapse is scored by how far
   it moves the surface, and the cheapest collapses go first.
3. **The setting picks the starting point.** `Ultra` loads mesh index 0 (full),
   `High` starts at index 1, and so on. LOD (see `lod_draw_distance.md`) then walks
   further down the chain with distance. In effect, model quality is a *global LOD bias*.
4. **Per-vertex payload** shrinks too: Low uses 2 bone weights instead of 4, packed
   16-bit normals and half-float UVs.

In this prototype Model Quality is applied to **every** scene primitive: LOD chains are generated at load
time by vertex clustering and Model Quality offsets the LOD that the projected-error rule picks
(Low +2 levels, Medium +1, High 0, Ultra 0 with half the error tolerance).

## Breakdown

**In this prototype:**

1. Stage `FrameSetup` — `filterDraws` of `geometry.lod`: for every item core put in the DrawList it picks
   `DrawItem::lod`; Model Quality adds a fixed offset (Low +2, Medium +1, High 0, Ultra 0) and scales the allowed
   projected error (1.5 / 1.25 / 1.0 / 0.5 px).
2. At load (and whenever the scene changes) every primitive with ≥ 200 triangles gets up to three extra LODs from
   `geo::simplifyClustered` (vertex clustering at 40 / 20 / 10 cells across the bounding box), stored with
   `Scene::addLod(prim, indices, cellSize)`. Every pass that draws the scene (G-buffer, shadows, forward) then uses the
   picked LOD automatically.


| Cost                | Scales with                                                   |
|---------------------|---------------------------------------------------------------|
| Vertex shader       | vertex count × skinning work                                  |
| Primitive assembly  | triangle count; GPUs cull ~2–4 tris/clock per raster engine    |
| Rasterizer          | **tiny triangles** are the killer: below ~8×8 px a GPU wastes quad lanes (2×2 shading quads), so a 1-px triangle can cost 4× its pixel |
| Memory / bandwidth  | VB/IB size, also shadow passes draw the mesh again per cascade |
| CPU                 | almost none; draw-call count does not change                  |

Visual impact: silhouettes (round objects look polygonal on Low), and lighting
artefacts on low meshes where normals are interpolated across big triangles.

## Optimization

- Pair with **LOD**: never draw the Ultra mesh at 200 m.
- Keep triangles ≥ ~10 px² on screen; let LOD select based on *projected size*, not distance.
- Run `meshopt_optimizeVertexCache` + `meshopt_optimizeOverdraw` + `meshopt_optimizeVertexFetch`
  on every mesh: same triangles, 10–30 % faster.
- Quantize: 16-bit positions (with per-mesh bounds), octahedral normals in 2×8 bit,
  half UVs. Halves vertex bandwidth.
- Use cheaper meshes in shadow passes (shadow LOD bias +1).
- For huge counts, move to **mesh shaders / meshlets** (Nanite-style clusters) with
  per-cluster culling.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers.

<!-- bench: geometry.meshQuality -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Model Quality | Low | -5.557 | -4.144 | -4.173 | +0.0 | +0.0 |
| Model Quality | Medium | +5.795 | +1.436 | +1.429 | +0.0 | +0.0 |
| Model Quality | High | +13.703 | +9.808 | +9.793 | +0.0 | +0.0 |
| Model Quality | Ultra | +66.713 | +65.827 | +65.811 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild (from scratch)

```cpp
// 1. Generate (or load) the highest detail mesh.
Mesh full = makeTorusKnot(512, 24);
// 2. Build a simplification chain (meshoptimizer).
std::vector<Mesh> chain{full};
for (float ratio : {0.5f, 0.25f, 0.125f}) {
    Mesh m = full;
    size_t target = size_t(full.indices.size() * ratio) / 3 * 3;
    float err;
    m.indices.resize(meshopt_simplify(m.indices.data(), full.indices.data(),
        full.indices.size(), &full.positions[0].x, full.positions.size(),
        sizeof(vec3), target, 1e-2f, 0, &err));
    chain.push_back(m);
}
// 3. The setting = which chain index LOD 0 uses.
int baseLod = (int[]){2, 1, 0, 0}[int(settings.meshQuality)];   // Low +2 ... Ultra 0 (+ half error tolerance)
```
Upload each chain entry to its own VBO/IBO (or one big buffer with offsets), and let
the LOD system add its own distance-based offset to `baseLod`.

In the prototype we skip meshoptimizer and use `geo::simplifyClustered` (vertex clustering, ~60 lines, no
dependencies): snap vertices to a grid of N cells across the bounding box, keep one representative vertex per cell
(closest to the cell average), drop triangles that collapse. Lower quality than quadric collapse, but robust and fast.
