# Culling (Frustum and Occlusion)

> Menu: **Performance → Frustum Culling / Occlusion Culling / Small Object Culling**
> Setting ids: `performance.frustumCulling`, `performance.occlusionCulling` (Off / On), `performance.smallObjectCulling` (0–8 px) · Owner module: `geometry.lod`
> Presets: Occlusion On for all presets · Small object 4 / 2 / 1 / 0.5 px
> Files: `src/modules/geometry/lod_culling.cpp` (`OcclusionBuffer`), `shaders/geometry/terrain.tesc` (patch culling)

## Definition

Culling means *not drawing things the camera cannot see*. **Frustum culling** skips
objects outside the camera's view pyramid. **Occlusion culling** skips objects that are
inside the view but hidden behind other objects (a house behind a hill). **Small-object
(contribution) culling** skips things that would cover fewer than N pixels. Games rarely
expose culling as a menu option (it is always on), but its quality knobs often hide
inside "draw distance", "object detail" or console variables, and turning it off is the
best way to understand why it matters.

## How it works

**Frustum culling.**
Extract the 6 planes from the view-projection matrix (Gribb–Hartmann method). For each
object's bounding sphere: if `dot(plane.xyz, center) + plane.w < -radius` for any plane,
the object is outside. AABBs use the "positive vertex" test. Hierarchies (BVH, octree)
reject whole groups at once.

**Occlusion culling, the modern GPU way (Hi-Z, two-phase):**
1. Draw last frame's visible objects first (they are probably still visible) → depth buffer.
2. Build a **hierarchical Z (Hi-Z) pyramid**: mip 0 = depth, each next mip stores the
   *farthest* depth of each 2×2 block (with reversed-Z that is the **min** value).
3. For every object (in a compute shader): project its bounding box to screen, pick the
   mip where the box covers about 2×2 texels, read those texels. If the object's
   *nearest* depth is still farther than the farthest stored depth there, something is
   fully in front of it → culled.
4. Objects that turned out visible but were not in step 1 are drawn in a second pass,
   then the pyramid is rebuilt for next frame.
The compute shader writes surviving objects into an **indirect draw buffer**
(`glMultiDrawElementsIndirect`), so the CPU never touches per-object visibility.

**Other techniques:** hardware occlusion queries (`GL_ANY_SAMPLES_PASSED`, one frame
late), software rasterized occluders on the CPU (Intel Masked Occlusion Culling,
used by many engines), precomputed visibility (PVS / portals for indoor levels), and
**cluster/meshlet culling** (per 64-triangle chunk, Nanite) including back-face cone tests.

## Breakdown

**In this prototype:**

1. Stage `FrameSetup` — `geometry.lod::filterDraws`. Frustum test = AABB "positive vertex" test against 5 planes
   (left, right, bottom, top, near; the reversed-Z projection is infinite so there is no far plane).
2. **Implemented occlusion = CPU software rasterization** (the Intel Masked Occlusion Culling idea, simplified):
   the 48 biggest visible objects (coarsest LOD) plus a 48×48 grid of the terrain are rasterized into a 256×144
   reversed-Z depth buffer, a 6-level min pyramid is built, and every AABB is tested against the mip where its
   screen rect is ~2×2 texels. Counters `cull.frustum / cull.occlusion / cull.small / cull.distance` show results.
3. Terrain patches are frustum-culled on the GPU in the hull shader (tess factor 0).
4. The GPU Hi-Z two-phase method described below is the reference design; it is not used in this module because
   the core DrawList is CPU-side.


| Technique          | Cost                             | Saves                                  |
|--------------------|----------------------------------|----------------------------------------|
| Frustum (CPU)      | ~nanoseconds per object, scales O(n) | typically 50–80 % of objects        |
| Frustum (GPU)      | one compute dispatch             | same, plus no CPU cost                 |
| Hi-Z occlusion     | pyramid build (~0.1 ms) + dispatch | huge in cities/indoors, ~0 in open fields |
| Occlusion queries  | GPU sync / 1-frame latency       | popping when turning fast              |
| Small object cull  | trivial                          | many tiny draw calls                   |

Culling saves **both** CPU (fewer draw calls) and GPU (fewer vertices, less overdraw,
fewer shadow casters). Shadow maps need their own culling from the light's frustum.

## Optimization

- Cull on the GPU and draw indirect; combine culling with LOD selection in the same
  compute pass (see `lod_draw_distance.md`).
- Two-phase Hi-Z avoids the one-frame lag and its popping.
- Conservative bounds: inflate by animation / displacement extents (tessellated terrain!).
- Cull per cluster (meshlets) for big meshes, not just per object.
- Separate culling per shadow cascade, and skip casters whose shadow can't reach the view.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers.

<!-- bench: performance.frustumCulling performance.occlusionCulling performance.smallObjectCulling -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Frustum Culling | Off | +1.277 | +4.046 | +4.047 | +0.1 | +0.0 |
| Frustum Culling | On | +11.366 | +11.618 | +11.647 | +0.0 | +0.0 |
| Occlusion Culling | Off | +12.019 | +16.944 | +16.926 | +0.0 | +0.0 |
| Occlusion Culling | On | +32.334 | +33.754 | +33.732 | +0.0 | +0.0 |
| Small Object Culling | 0.0 px | -14.482 | -7.293 | -7.304 | +0.0 | +0.0 |
| Small Object Culling | 1.0 px | +7.671 | +7.255 | +7.274 | +0.0 | +0.0 |
| Small Object Culling | 8.0 px | +13.590 | +12.861 | +12.862 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild (from scratch)

```cpp
// Gribb–Hartmann frustum plane extraction (works for D3D-style 0..1 depth too:
// near plane = row2 only, far = row3 - row2).
void extractPlanes(const glm::mat4& m, glm::vec4 out[6]) {
    glm::mat4 t = glm::transpose(m);
    out[0] = t[3] + t[0]; out[1] = t[3] - t[0];   // left, right
    out[2] = t[3] + t[1]; out[3] = t[3] - t[1];   // bottom, top
    out[4] = t[2];        out[5] = t[3] - t[2];   // near, far (zero-to-one clip)
    for (int i = 0; i < 6; ++i) out[i] /= glm::length(glm::vec3(out[i]));
}
bool sphereVisible(const glm::vec4 planes[6], glm::vec3 c, float r) {
    for (int i = 0; i < 6; ++i)
        if (glm::dot(glm::vec3(planes[i]), c) + planes[i].w < -r) return false;
    return true;
}
```
With reversed-Z the near/far rows swap meaning: `t[3] - t[2]` becomes the **near** plane
and `t[2]` the far plane (degenerate with an infinite far plane). The prototype therefore
tests the 4 side planes plus `t[3] - t[2]` and handles draw distance separately.

Hi-Z build (reversed-Z → keep the minimum = farthest):
```glsl
layout(local_size_x=8, local_size_y=8) in;
layout(binding=0) uniform sampler2D uSrc;               // previous mip
layout(r32f, binding=1) uniform writeonly image2D uDst;
void main(){ ivec2 p = ivec2(gl_GlobalInvocationID.xy); ivec2 s = p*2;
  float a=texelFetch(uSrc,s,0).r, b=texelFetch(uSrc,s+ivec2(1,0),0).r,
        c=texelFetch(uSrc,s+ivec2(0,1),0).r, d=texelFetch(uSrc,s+ivec2(1,1),0).r;
  imageStore(uDst, p, vec4(min(min(a,b),min(c,d)))); }
```
Occlusion test per object: project the 8 AABB corners, take the screen rect and the
*maximum* depth (nearest, reversed-Z), choose `mip = ceil(log2(max(w,h)))`, sample the
4 texels covering the rect; culled if `nearestObjDepth < min(texels)`.
