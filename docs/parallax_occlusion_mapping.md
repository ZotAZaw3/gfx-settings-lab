# Parallax Occlusion Mapping (POM)

Menu: **Geometry → Parallax Occlusion** (`Off | Low | Medium | High`),
**POM Depth** (0 – 0.1), **Self-shadowing** (on/off)
Code: `GeoVfxSettings::pom`, `pomDepth`, `pomShadows`; shader `pom.frag`

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
