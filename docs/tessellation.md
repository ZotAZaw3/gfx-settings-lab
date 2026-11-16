# Tessellation and Displacement

> Menu: **Geometry → Tessellation / Displacement Strength** (+ debug: Patch Frustum Culling, Show Terrain Triangles)
> Setting ids: `geometry.tessellation` (Off/Low/Medium/High/Ultra), `world.terrainDisplacement` (0–1 → 0–30 m), `geometry.tessPatchCulling`, `geometry.showTriangles` · Owner module: `geometry.terrain`
> Presets: Low=Off, Medium=Medium, High=High, Ultra=Ultra
> Files: `src/modules/geometry/terrain.cpp`, `shaders/geometry/terrain.{vert,tesc,tese,frag}`, `terrain_params.glsl`

## Definition

Hardware tessellation subdivides coarse patches into many small triangles **on the GPU,
every frame**, and displacement then pushes the new vertices along the normal using a
height map. The result: real geometric bumps (rocks, bricks, terrain, ocean waves) with
correct silhouettes and self-shadowing, from a cheap low-poly input mesh. "Tessellation"
in a graphics menu almost always means this hardware tessellation + displacement.

## How it works

The pipeline gains three stages between the vertex and fragment shader:

```
VS (per control point) → TCS / Hull (per patch: decide how much) → fixed-function
tessellator (generate barycentric/uv points) → TES / Domain (place each new vertex,
sample height map, displace) → [GS] → raster → FS
```

1. **Patches.** Draw with `GL_PATCHES` (`glPatchParameteri(GL_PATCH_VERTICES, 4)` for quads).
2. **Tessellation Control Shader** computes `gl_TessLevelOuter[4]` and
   `gl_TessLevelInner[2]`. Good TCSs are *adaptive*:
   - **Edge screen length**: project both ends of each edge, `level = edgePx / targetPx`.
     Shared edges compute the same value from the same two vertices, so no cracks.
   - **Frustum cull** the patch (set levels to 0) to discard it before generating anything.
   - Optional: scale by height-map variance (flat areas need no detail).
3. **Tessellator** (fixed-function, not programmable) generates up to 64 × 64 subdivisions
   per patch. `fractional_odd_spacing` gives smooth level changes (no popping).
4. **Tessellation Evaluation Shader** interpolates the patch corners at `gl_TessCoord`,
   samples the height map (`textureLod`, because there are no derivatives here), and
   moves the vertex: `pos += normal * height * strength`.
5. **Normals** must be recomputed from the height map (central differences) in the TES
   or FS, otherwise lighting stays flat.

D3D11/12 names: Hull shader = TCS, Domain shader = TES. `SV_TessFactor` = `gl_TessLevelOuter`.

## Breakdown

**In this prototype:**

1. Stage `GBuffer`, order 10 — pass "Terrain": draws N×N 4-point patches (`Topology::Patches`) into the core
   G-buffer (albedo, normal, material, emissive, velocity + scene.depth). Tessellation **Off** draws a static grid
   with 8 quads per patch edge, displaced in the vertex shader instead.
2. Level per setting: max factor 1 / 8 / 16 / 32 / 64 and target edge length – / 24 / 16 / 10 / 6 px, capped
   further by Terrain Quality (16 / 32 / 64 / 64).
3. Disabled (greyed out) when `Caps::tessellation` is false (the D3D11 cross-compile path).


| Setting | Max level | Target px | Typical cost |
|---------|-----------|-----------|--------------|
| Off     | 1         | –         | plain mesh (normal map only) |
| Low     | 8         | 24        | small        |
| Medium  | 16        | 16        | moderate     |
| High    | 32        | 10        | high         |
| Ultra   | 64        | 6         | very high, tiny triangles |

Costs:
- **Tessellator throughput** is fixed per GPU; high factors bottleneck the front end.
- **Tiny triangles** (< 8 px) waste rasterizer quads, the same problem as over-detailed meshes.
- **Shadow passes** tessellate again for every cascade/cube face.
- Fragment cost barely changes, so a pixel-bound game sees little impact; a geometry-bound
  one can lose 30 %+.

## Optimization

- Always **adaptive by screen-space edge length**, never a constant factor.
- **Cull patches in the TCS** (frustum and back-facing) by setting outer levels to 0.
- Clamp the maximum factor and the minimum triangle size (`tessTargetPx`).
- Lower tessellation in shadow passes (shadows rarely need the detail).
- Use **POM** (`parallax_occlusion_mapping.md`) for small-scale detail and tessellation only
  for large-scale shape. Many modern engines replaced tessellation with **mesh shaders** or
  **Nanite-style** pre-tessellated clusters, which avoid the fixed-function bottleneck.
- Distance fade the displacement strength to 0 so far patches can stay at level 1.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers.

<!-- bench: geometry.tessellation world.terrainDisplacement -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Tessellation | Off | +7.859 | +4.616 | +4.641 | +0.0 | +0.1 |
| Tessellation | Low | -20.230 | -17.945 | -17.947 | +0.0 | +0.1 |
| Tessellation | Medium | -10.828 | -6.000 | -5.998 | +0.0 | +0.1 |
| Tessellation | High | +11.243 | +11.762 | +11.776 | +0.0 | +0.1 |
| Tessellation | Ultra | +29.951 | +33.684 | +33.691 | +0.0 | +0.1 |
| Displacement Strength | 0.00 | -8.880 | -7.330 | -7.340 | +0.0 | +0.0 |
| Displacement Strength | 1.00 | -5.408 | -7.516 | -7.528 | +0.0 | +0.0 |
| Displacement Strength | 1.00 | -40.318 | -44.800 | -44.835 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild (from scratch)

```glsl
// terrain.tesc
layout(vertices = 4) out;
uniform mat4 uViewProj; uniform vec2 uViewport; uniform float uTargetPx, uMaxLevel;
in vec3 vPos[]; out vec3 tcPos[];
float edgeLevel(vec3 a, vec3 b){
    vec4 pa = uViewProj*vec4(a,1), pb = uViewProj*vec4(b,1);
    vec2 sa = pa.xy/pa.w*0.5*uViewport, sb = pb.xy/pb.w*0.5*uViewport;
    return clamp(distance(sa,sb)/uTargetPx, 1.0, uMaxLevel);
}
void main(){
    tcPos[gl_InvocationID] = vPos[gl_InvocationID];
    if (gl_InvocationID == 0) {
        // frustum cull omitted here; see the module source
        gl_TessLevelOuter[0] = edgeLevel(vPos[3], vPos[0]);
        gl_TessLevelOuter[1] = edgeLevel(vPos[0], vPos[1]);
        gl_TessLevelOuter[2] = edgeLevel(vPos[1], vPos[2]);
        gl_TessLevelOuter[3] = edgeLevel(vPos[2], vPos[3]);
        gl_TessLevelInner[0] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
        gl_TessLevelInner[1] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);
    }
}
// terrain.tese
layout(quads, fractional_odd_spacing, ccw) in;
uniform sampler2D uHeight; uniform float uDisp; uniform mat4 uViewProj;
in vec3 tcPos[]; out vec3 tePos; out vec2 teUV;
void main(){
    vec2 t = gl_TessCoord.xy;
    vec3 p = mix(mix(tcPos[0],tcPos[1],t.x), mix(tcPos[3],tcPos[2],t.x), t.y);
    teUV = p.xz * 0.02;
    p.y += textureLod(uHeight, teUV, 0.0).r * uDisp;
    tePos = p; gl_Position = uViewProj * vec4(p,1);
}
```
C++ side: `glPatchParameteri(GL_PATCH_VERTICES, 4); glDrawArrays(GL_PATCHES, 0, n);`.
When the setting is `Off`, bind a program without the TCS/TES and draw the same grid as
triangles with displacement done in the vertex shader at grid resolution.
