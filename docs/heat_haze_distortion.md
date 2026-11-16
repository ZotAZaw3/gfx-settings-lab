# Heat Haze & Screen Distortion

> Menu: **Post Processing → Lens & Screen → Heat haze** · Setting id(s): `post.haze`, `post.haze.strength/speed/start` · Owner module: `post`
> Values: On/Off + sliders · Presets: Off by default

## Definition

Hot air above asphalt, fire or a jet engine has a different refractive index
than the cooler air around it, so the scene behind it shimmers and wobbles.
The same "distortion" technique powers shockwaves, invisibility cloaks and
underwater warping.

## How it works

1. **Distortion buffer.** Effects that distort (a fire's particles, a
   shockwave ring) draw into a small `RG16F` buffer the *UV offset* they want
   instead of a color. In this prototype a global haze is computed
   procedurally: animated noise, masked to distant ground (from depth) so
   haze appears near the horizon on hot days.
2. **Mask by depth.** Only pixels *behind* the distorting volume should be
   shifted; when sampling the offset location, check its depth is farther
   than the distorter, otherwise use the undistorted color (prevents
   foreground objects "leaking" into the haze).
3. **Offset the lookup.** `color = scene(uv + offset)`.
4. Optional chromatic split: sample R/G/B with slightly different offsets.

Stage: **PostRender** (needs depth; runs before DoF so the haze itself can be
out of focus).

## Breakdown

| Piece | Where |
|---|---|
| Procedural haze | `heat_haze.frag` (2 octaves scrolling value noise) |
| Depth mask | linear depth from reversed-Z, `smoothstep(start, end, z)` |
| Particle distortion buffer | hook for the VFX module: `ctx.post` reads an optional `scene.distortion` texture if the VFX thread provides one |

## Optimization

One pass, 2–3 fetches. Use half-res distortion buffer (offsets are smooth).
Skip when strength is 0 and no particle wrote distortion this frame.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.haze` | Off | +5.9 | +3.4 | +0.0 | n/a |
| `post.haze` | On | +29.8 | +27.6 | +0.0 | n/a |

## Rebuild (from scratch)

```glsl
float z = linearDepth(texture(uDepth, uv).r);
float m = smoothstep(uStart, uEnd, z) * uStrength;
vec2 n = vec2(noise(uv*uScale + vec2(0, uTime*uSpeed)),
              noise(uv*uScale*1.7 - vec2(uTime*uSpeed*0.7, 0))) - 0.5;
vec2 suv = uv + n * m * 0.01;
if (linearDepth(texture(uDepth, suv).r) < z*0.9) suv = uv; // foreground guard
out = texture(uScene, suv);
```

Files: `src/modules/post/post_module.cpp` (runHaze), `shaders/post/heat_haze.frag`
