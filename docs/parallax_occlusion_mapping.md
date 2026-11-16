# Parallax Occlusion Mapping (POM)

> Menu: **Geometry → Parallax Occlusion Mapping / POM Depth**
> Setting ids: `geometry.pom` (Off / Low / Medium / High), `geometry.pomDepth` (0–0.15) · Owner module: `geometry.terrain`
> Presets: Low=Off, Medium=Low, High=Medium, Ultra=High · Self-shadowing is part of Medium (8 steps) and High (16 steps)
> Files: `src/modules/geometry/terrain.cpp` (`drawPom`), `shaders/geometry/pom.{vert,frag}`, `pom_params.glsl`

## Definition

POM fakes depth on a flat surface by ray-marching a height map *inside the pixel
shader*. Each pixel works out which point of the bumpy surface the eye would really hit
and shades that point instead. Bricks, cobblestones and tiles appear to stick out and
hide each other, without adding a single triangle. It is often grouped with
"Texture quality" or "Surface detail" in game menus.

## How it works

Family tree, cheapest to best:
1. **Normal mapping**: changes lighting only. Flat at grazing angles.
2. **Parallax mapping** (one sample): shifts UVs by `viewDir.xy / viewDir.z * h * scale`.
   Cheap, swims at grazing angles.
3. **Steep parallax / POM**: march the view ray through the height field in N layers until
   it goes below the surface, then linearly interpolate between the last two samples
   (that interpolation step is what makes it "occlusion" mapping).
4. **Relief mapping**: adds a binary search refinement after the linear march.

Algorithm (tangent space):
```
V  = normalize(viewDir in tangent space)
layers = mix(maxLayers, minLayers, abs(V.z))   // more steps at grazing angles
step   = 1 / layers;  deltaUV = V.xy / V.z * depthScale / layers
march: uv -= deltaUV; depth += step; until depth >= heightMap(uv) (stored as depth)
refine: lerp between previous and current sample by their errors
```
Then the shader uses the new `uv` for every texture (albedo, normal, roughness).
**Self-shadowing**: march again from the hit point toward the light; if the ray goes
under the height field, darken.

Limitations: silhouettes stay flat (the polygon edge is still straight), and it breaks
on strongly curved surfaces. Use tessellation for silhouettes.

## Breakdown

**In this prototype:**

1. Stage `GBuffer`, order 12 — pass "POM floor": a 12 × 12 m procedural brick floor on the plaza
   (512² R8 depth map + RGBA8 albedo, both mipmapped). Off = the same depth map used only as a normal source.
2. The mip level is computed once from the original UV derivatives and passed to `textureLod` inside the loop
   (portable to the D3D11 cross-compile, avoids undefined derivatives in divergent loops).
3. Self-shadow darkens albedo (the G-buffer has no per-pixel sun mask a module may write) and cavity depth goes
   into material AO.


| Level  | Min / max layers | Refinement | Shadow steps |
|--------|------------------|------------|--------------|
| Off    | – (normal map)   | –          | –            |
| Low    | 8 / 16           | linear     | 0            |
| Medium | 12 / 32          | linear     | 8            |
| High   | 16 / 64          | linear + 5 binary | 16    |

Cost is **pure pixel shader + texture fetch**, proportional to covered pixels × steps.
Dependent texture reads in a loop can be slow on GPUs with high latency. No vertex
or CPU cost, so it scales with resolution (and benefits a lot from upscaling).

## Optimization

- Fewer layers when looking straight down (`abs(V.z)` high), more at grazing angles.
- Fade POM out with distance / mip level; far pixels fall back to normal mapping.
- Use `textureGrad` with the *original* UV derivatives so mip selection stays correct and
  fast inside the loop (derivatives are undefined inside divergent loops).
- Store height in the alpha of the normal map to halve fetches.
- Write corrected depth (`gl_FragDepth`) only if you need correct intersections with
  other geometry; it disables early-Z, so avoid if possible.
- Use cone step mapping / precomputed distance fields for big speedups on deep surfaces.

### Measured cost (llvmpipe, relative only)

Measured with `gfxlab --bench-settings` on Mesa llvmpipe (a CPU software renderer), 640×360, 5 warm-up + 30 measured frames per value, showcase camera on the terrain overview. Each row is the change versus the High preset with only that one setting moved. On llvmpipe the GPU work runs on the CPU, so GPU and CPU columns move together, and two runs of the same configuration differed by up to ~35 ms, so treat anything under ~30 ms as noise. Use the table for direction and rough ratios only; rerun `--bench-settings` on real hardware for actual numbers.

<!-- bench: geometry.pom geometry.pomDepth -->
Measured on `llvmpipe  LLVM 20 1 2  256 bits  geometry` (deltas vs the High preset; from `llvmpipe__LLVM_20_1_2__256_bits__geometry.csv`):

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|---|
| Parallax Occlusion Mapping | Off | +20.889 | +11.994 | +12.054 | +0.0 | +0.0 |
| Parallax Occlusion Mapping | Low | -3.755 | -8.305 | -8.319 | +0.0 | +0.0 |
| Parallax Occlusion Mapping | Medium | +13.195 | +7.811 | +7.835 | +0.0 | +0.0 |
| Parallax Occlusion Mapping | High | +15.392 | +8.273 | +8.274 | +0.0 | +0.0 |
| POM Depth | 0.00 | +10.909 | +2.759 | +2.752 | +0.0 | +0.0 |
| POM Depth | 0.06 | +2.233 | -3.947 | -3.973 | +0.0 | +0.0 |
| POM Depth | 0.15 | +7.414 | +0.597 | +0.613 | +0.0 | +0.0 |
<!-- /bench -->

## Rebuild (from scratch)

```glsl
vec2 pomUV(sampler2D depthMap, vec2 uv, vec3 V, float scale, float minL, float maxL){
    float layers = mix(maxL, minL, abs(V.z));
    float step = 1.0 / layers, cur = 0.0;
    vec2 dUV = V.xy / max(V.z, 0.05) * scale / layers;
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    float d = textureGrad(depthMap, uv, dx, dy).r;
    while (cur < d) { uv -= dUV; d = textureGrad(depthMap, uv, dx, dy).r; cur += step; }
    vec2 prev = uv + dUV;
    float after = d - cur;
    float before = textureGrad(depthMap, prev, dx, dy).r - cur + step;
    float w = after / (after - before);
    return mix(uv, prev, w);
}
```
Build a TBN in the vertex shader, transform the view vector into tangent space, call
`pomUV`, then sample all material textures at the returned UV. In the prototype the
height map is generated procedurally (bricks) at startup.
