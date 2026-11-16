# Model / Mesh Quality

Menu: **Geometry → Model Quality** (`Low | Medium | High | Ultra`)
Code: `src/modules/geometry/` → `GeoVfxSettings::meshQuality`, `MeshLibrary::build()`

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

In this prototype the meshes are procedural (UV sphere, torus-knot, terrain grid) so
"quality" is the tessellation of the generator:

| Level  | Sphere seg × rings | Torus knot seg × sides | Terrain grid |
|--------|--------------------|------------------------|--------------|
| Low    | 12 × 8             | 64 × 6                 | 32²          |
| Medium | 24 × 16            | 128 × 10               | 64²          |
| High   | 48 × 32            | 256 × 16               | 128²         |
| Ultra  | 96 × 64            | 512 × 24               | 256²         |

## Breakdown

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
int baseLod = 3 - int(settings.meshQuality);   // Ultra=0 ... Low=3
```
Upload each chain entry to its own VBO/IBO (or one big buffer with offsets), and let
the LOD system add its own distance-based offset to `baseLod`.

In the prototype we skip meshoptimizer and just regenerate the procedural mesh at the
table's resolution when the setting changes (`MeshLibrary::build(quality)`).
