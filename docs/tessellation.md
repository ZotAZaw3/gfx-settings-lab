# Tessellation and Displacement

Menu: **Geometry → Tessellation** (`Off | Low | Medium | High | Ultra`),
**Displacement Strength** (0 – 1), **Target Triangle Size** (px, 4 – 32)
Code: `GeoVfxSettings::tessellation`, `displacement`, `tessTargetPx`;
shaders `terrain.vert/.tesc/.tese/.frag`

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
