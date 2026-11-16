# God Rays (Light Shafts / Crepuscular Rays)

> Menu: **Post Processing → God Rays** · Setting id(s): `post.rays`, `post.rays.intensity/density/decay/weight` · Owner module: `post`
> Values: On/Off; samples 8/12/16/24 per pass from `post.quality` · Presets: Low=Off, else On

## Definition

God rays are visible beams of light streaming through gaps, such as sunlight
through tree leaves, windows, or clouds. In reality, light scatters off dust,
fog and water droplets in the air; where light is blocked, that air stays dark,
so the lit air forms visible shafts. Games either fake this in screen space
(cheap, only works when the light source is on or near the screen) or simulate
it volumetrically (expensive, correct from any angle).

## How it works

**Screen-space radial blur** (Mitchell, "Volumetric Light Scattering as a
Post-Process", GPU Gems 3, ch. 13):

1. **Occlusion mask** (half res): pixels that see the sky (depth == far
   plane, i.e. `0.0` in reversed-Z) get the sky/sun color; everything else is
   black. A soft disc around the sun's screen position boosts the source.
2. **Radial blur toward the sun.** For each pixel march `N` samples from the
   pixel toward the sun's screen position. Accumulate the mask with
   exponentially decaying weight:
   ```
   delta = (uv - sunUV) * density / N
   illum = 1
   for i in N: uv -= delta; sum += mask(uv) * illum * weight; illum *= decay
   ```
   Bright gaps "smear" outward along the direction away from the sun,
   occluders smear dark shadows: shafts appear.
3. **Fade** by how much the camera faces the sun (`dot(viewDir, sunDir)`),
   and by the sun's distance from the screen, so the effect disappears
   smoothly when the sun goes off-screen instead of popping.
4. **Composite** additively into HDR, before bloom so shafts also bloom.

**Volumetric** (for reference): ray-march from the camera through a froxel
grid (frustum-aligned 3D texture, e.g. 160×90×64), sampling the shadow map at
each step and accumulating in-scattering with a phase function
(Henyey-Greenstein). Works for any light and from any angle, costs 1–3 ms.

## Breakdown

| Pass | Shader | Res |
|---|---|---|
| Occlusion mask | `godrays_mask.frag` | 1/2 |
| Radial blur | `godrays_blur.frag` (run twice with smaller step for quality presets: "two-pass" makes 64 effective samples from 2×8) | 1/2 |
| Composite | PostOutput composite | full |

## Optimization

- Half or quarter resolution: shafts are smooth.
- **Two-pass radial blur**: 2 passes of 8 samples = 64 effective samples.
- Per-pixel jitter on the start offset + TAA removes banding.
- Skip when `dot(viewDir, sunDir) < 0` (sun behind the camera).

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.rays` | Off | -1.8 | -1.7 | +0.0 | n/a |
| `post.rays` | On | -6.4 | -5.1 | +0.0 | n/a |

## Rebuild (from scratch)

1. Get sun screen position: `p = proj * view * vec4(camPos + sunDir*1e4, 1)`,
   `sunUV = p.xy/p.w * vec2(0.5,-0.5) + 0.5` (top-left origin).
2. Mask shader: `mask = (depth == 0.0) ? skyColor : vec3(0)` plus
   `sunColor * smoothstep(r, 0, distance(uv*aspect, sunUV*aspect))`.
3. Blur shader: the loop above, with `density≈0.9, decay≈0.96, weight≈0.4, N=32`.
4. Composite: `hdr += rays * intensity * fade`.
5. D3D11: identical.

Files: `src/modules/post/post_module.cpp` (runRays), `shaders/post/godrays_mask.frag`, `shaders/post/godrays_blur.frag`, `shaders/post/composite_output.frag`
