# Virtualized Geometry (Nanite-style) and Mesh Shaders

Menu: **Geometry → Geometry Pipeline** (`Classic | Meshlet Culling (compute)`),
**Cluster Error Threshold** (px, 0.5 – 4)
Code: `GeoVfxSettings::meshletMode`, `clusterErrorPx`; `MeshletBuilder`, shader
`geometry/meshlet_cull.comp` (compute emulation of task/mesh shaders)

## Definition

**Mesh shaders** (D3D12 Ultimate, Vulkan `VK_EXT_mesh_shader`, OpenGL `GL_NV_mesh_shader`)
replace the old vertex → tessellation → geometry pipeline with a compute-like stage that
outputs small batches of triangles (**meshlets**, ~64 vertices / 124 triangles) directly
to the rasterizer, with an optional **task/amplification shader** in front that decides
which meshlets to launch.

**Virtualized geometry** (Unreal 5 Nanite and similar) builds on the same idea: every mesh
is pre-split into clusters arranged in a hierarchy of simplified versions, and each frame
the GPU picks exactly the clusters needed so triangles are about one pixel in size. Film
-quality assets with millions of triangles can be used directly, and traditional LODs
disappear. Games expose it as "Nanite on/off", "Geometry detail", or it is implicit in
quality presets.

## How it works

**Meshlets / mesh shaders:**
1. Offline: split each mesh into meshlets (meshoptimizer `meshopt_buildMeshlets`), store
   each meshlet's bounding sphere and **normal cone** (all triangle normals inside a cone).
2. Task shader (one thread per meshlet): frustum test the sphere, back-face test the cone
   (if the camera sees all triangles from behind, skip), occlusion test against Hi-Z,
   and LOD pick. Emit surviving meshlet IDs.
3. Mesh shader (one workgroup per meshlet): load vertices, transform them, write up to
   124 triangles to shared output. No input assembler, no index buffer fetch bottleneck.

**Nanite-style virtualized geometry:**
1. Offline: cluster the mesh (128 triangles), then group clusters, simplify each group to
   half the triangles while *locking group boundaries*, re-cluster, repeat → a DAG of
   clusters from full detail up to a handful of triangles. Each cluster stores its
   simplification error and its parent's error.
2. Runtime LOD cut: a cluster is drawn if its own projected error ≤ threshold **and** its
   parent's projected error > threshold. Because the decision depends only on the cluster
   and its parent, it runs in parallel with no cracks.
3. Two-phase Hi-Z occlusion culling per cluster (see `culling.md`).
4. **Software rasterization** of tiny triangles in a compute shader (writing 64-bit
   depth+ID atomics into a visibility buffer), hardware rasterization for big ones.
5. **Visibility buffer**: the raster pass writes only triangle ID; materials are shaded
   afterwards per pixel, so overdraw costs almost nothing.
6. Geometry is **streamed** in pages like virtual textures.

## Breakdown

| Mode                  | Culling granularity | LOD granularity | Needs                    |
|-----------------------|---------------------|-----------------|--------------------------|
| Classic               | per object          | per object chain| any GPU                  |
| Meshlet culling       | per 64-tri cluster  | per object      | compute (+ mesh shaders for speed) |
| Virtualized (Nanite)  | per cluster         | per cluster     | compute, 64-bit atomics, streaming |

Cost profile: almost constant per frame relative to screen resolution (about one
triangle per pixel), large disk footprint, culling dispatches. Struggles with
foliage/alpha-masked materials and heavy deformation (improving in later versions).

## Optimization

- Cluster error threshold ≈ 1 px; raise it to trade detail for speed.
- Combine with an upscaler: lower internal resolution means fewer pixels and fewer clusters.
- Keep cluster bounds tight; normal cones discard ~25–50 % of meshlets on closed meshes.
- Use the visibility buffer to decouple material cost from geometry.

## Rebuild (from scratch, compute emulation used in the prototype)

1. On load, split each mesh into 64-triangle meshlets (greedy: walk triangles in index
   order after `optimizeVertexCache`, start a new meshlet when 64 tris or 64 unique verts
   are reached). Store per meshlet: `{indexOffset, indexCount, center, radius, coneAxis, coneCutoff}`.
2. Each frame, `meshlet_cull.comp` (one thread per meshlet) tests frustum + cone and, if
   visible, appends a `DrawElementsIndirectCommand {count, 1, firstIndex, 0, 0}` with an
   atomic counter.
3. `glMultiDrawElementsIndirectCount` (GL 4.6 / ARB_indirect_parameters) or
   `glMultiDrawElementsIndirect` with the max count and zeroed commands for culled ones.

```glsl
bool coneVisible(vec3 center, vec3 axis, float cutoff, vec3 cam){
    return dot(normalize(center - cam), axis) < cutoff;   // meshopt convention
}
```
On D3D12 the same data feeds a real amplification + mesh shader pair
(`DispatchMesh(meshletCount/32, 1, 1)`).
