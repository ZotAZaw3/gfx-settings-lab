# Models (glTF loading) and the Scene selector

> Menu: **Graphics → Scene** · Setting id: `scene.select` · Owner: core (`src/core/scene.cpp`)
> Related: [mesh_quality.md](mesh_quality.md), [lod_draw_distance.md](lod_draw_distance.md) (geometry module)

## Definition
Models are the 3D meshes in the world: their vertices (position, normal, tangent, UV), triangles,
materials and textures. The Scene selector loads either the built-in procedural test scene (spheres with
a roughness/metal grid, brick ground with height map for tessellation/POM, foliage cards, a mirror,
glass, emissive and subsurface materials) or any glTF 2.0 file placed under `assets/`.

## How it works
glTF 2.0 is the "JPEG of 3D": JSON describing nodes, meshes and materials plus binary buffers and
images. The loader (cgltf):
1. Decodes images (stb_image) to RGBA8 and builds full mip chains; base color/emissive are sRGB,
   normal/metal-roughness/occlusion are linear. The same image used both ways becomes two textures.
2. Converts materials to the engine's PBR metal-roughness `Material` (base color, metallic, roughness,
   normal scale, occlusion, emissive + `KHR_materials_emissive_strength`, alpha mode/cutoff, double
   sided; `unlit`, `clearcoat`, `transmission`/`volume` map to shading models).
3. Appends every triangle primitive's vertices to **one shared vertex buffer** and indices to **one
   shared index buffer** (`Primitive::baseVertex`, `lods[0].firstIndex/indexCount`). Missing normals are
   generated from faces; missing tangents with per-vertex accumulation (MikkTSpace is the reference).
4. Walks the node hierarchy to produce `Instance`s with world matrices and world AABBs, and reads
   `KHR_lights_punctual` lights (directional → sun).

A single vertex/index buffer means: one bind for the whole scene, vertex pulling and BVH builds can
read everything as SSBOs (RT module), and LOD levels are just extra index ranges (`Scene::addLod`).

## Breakdown
| Data | CPU | GPU |
|---|---|---|
| `Vertex` 48 B (pos, normal, tangent, uv) | `scene.vertices` | `vertexBuffer` (VB + SSBO 2) |
| indices u32 | `scene.indices` | `indexBuffer` (IB + SSBO 3) |
| materials | `scene.materials` | `materialBuffer` SSBO 1 (`GpuMaterial`, 112 B) |
| lights | `scene.lights` | `lightBuffer` SSBO 0 |
| textures | full mip chains in RAM | streamed (see texture_streaming.md) |

Drawing: `SceneRenderer::drawScene` writes one `DrawConstants` (world, prevWorld, viewProj, ids) per
draw into a 4 MB UBO ring with a single upload, then binds ranges per draw and switches pipelines only
when the material's alpha mode / sidedness changes.

## Optimization
- CPU: one `glDrawElementsBaseVertex` per primitive; ~10k draws/frame is fine on GL with this setup.
  Multi-draw-indirect (all draws in one call, per-draw data in an SSBO indexed by `gl_DrawID`) is the next
  step and what GPU-driven renderers use.
- RAM: CPU copies of geometry and texture mips are kept for BVH builds, LOD generation and streaming;
  shown as "scene (CPU copies)" in the overlay.
- VRAM: 48 B/vertex; quantized formats (16-bit positions/UVs, octahedral normals) can halve that.
- Load time: decode + mip generation are single-threaded; a production loader runs them on worker
  threads and loads pre-baked compressed textures (KTX2/Basis, DDS/BC7).

<!-- bench: scene.select -->
<!-- /bench -->

## Rebuild
1. Parse glTF with cgltf/tinygltf; load buffers.
2. For each primitive read POSITION/NORMAL/TANGENT/TEXCOORD_0 and indices into your vertex/index arrays.
3. Generate normals/tangents if missing; compute bounds.
4. Traverse nodes accumulating `parent × local` to get world transforms per mesh instance.
5. Map materials to your PBR inputs; mark color textures sRGB.
6. Upload once into big buffers; draw with base vertex/first index; verify with the Khronos sample models
   (Sponza, DamagedHelmet, MetalRoughSpheres).

Files: `src/core/scene.cpp`, `src/core/renderer.cpp`
