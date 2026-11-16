# Color Grading & LUT

> Menu: **Post Processing → Color Grading** · Setting id(s): `post.grade`, `post.grade.temperature/tint/contrast/saturation/vibrance/hue/lift/gamma/gain/split/strength`, `post.grade.showLut` · Owner module: `post`
> Values: On/Off + sliders · Presets: On at every preset

## Definition

Color grading is the artistic adjustment of an image's colors after it has been
exposed and tonemapped: making a scene warmer, cooler, more contrasty, giving
shadows a teal tint and highlights an orange one, desaturating for a bleak
mood. A **LUT (look-up table)** is a precomputed 3D texture that maps any input
color to an output color, so an arbitrarily complex grade (even one made in
Photoshop/DaVinci Resolve) costs one texture fetch per pixel.

## How it works

1. **Build the LUT** (only when a grading setting changes): a 32×32×32
   `RGBA8`/`RGB10A2` 3D texture. Each texel's coordinate *is* an input color
   `(r,g,b) = texelIndex / 31`. A shader renders each of the 32 slices,
   applying the full grading math to that input color and writing the result.
2. **Grading math, in order** (all in linear space unless noted):
   - White balance: convert to LMS cone space, scale by the ratio between the
     chosen and the reference white (Bradford-style von Kries adaptation),
     convert back.
   - Contrast: around middle grey in log space:
     `log = (log2(c) - log2(0.18)) * contrast + log2(0.18)`.
   - Lift/Gamma/Gain (ASC-CDL-like): `c = pow(max(c*gain + lift*(1-c), 0), 1/gamma)`.
   - Saturation: `mix(vec3(luma), c, saturation)`; vibrance boosts only
     low-saturation pixels.
   - Hue shift: rotate in YIQ / hue space.
3. **Apply.** The PostLDR pass samples `texture(uLUT, color * (31/32) + 0.5/32)`
   (the scale/offset maps 0 and 1 to texel *centers*, otherwise the extremes are
   half a texel wrong) and blends with the original by LUT strength.
4. **External LUTs.** Artists grade a screenshot with a neutral LUT strip
   (32 slices laid out 1024×32) in an image editor; the edited strip loaded back
   as a 3D texture reproduces the grade exactly.

## Breakdown

| Piece | Where |
|---|---|
| LUT generation | `shaders/post/lut_gen.frag`, one draw per Z slice into a 3D texture layer via `glFramebufferTexture3D` / `glFramebufferTextureLayer` |
| Dirty flag | `PostModule::buildLut` hashes the grading parameters; the LUT is rebuilt only when they change |
| Apply | `postldr.frag` (PostLDR stage), after chromatic aberration, before vignette/grain |
| Debug | "Show LUT" draws the 32 slices as a strip in the corner |

## Optimization

- The LUT costs nothing per frame (rebuilt only on change, 32 tiny draws).
- 16³ is often enough for subtle grades; 32³ for strong ones; 64³ is wasteful.
- Grade *before* tonemap with a log-encoded input (a "HDR LUT") if you want
  grading to behave the same at every exposure; this prototype grades in
  display-referred (post-tonemap) space, which is simpler and what most PC
  games expose as "LDR LUT".
- Using `GL_LINEAR` filtering on the 3D texture gives trilinear interpolation
  for free; do not do it manually.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.grade` | Off | -6.6 | -7.0 | +0.0 | n/a |
| `post.grade` | On | -3.0 | -3.0 | +0.0 | n/a |

## Rebuild (from scratch)

1. `glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 32,32,32, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0)`,
   `GL_LINEAR`, `GL_CLAMP_TO_EDGE` on R/S/T.
2. For `z` in 0..31: `glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, lut, 0, z)`,
   viewport 32×32, draw fullscreen with the slice index in a small per-draw UBO (`pPass.x`).
3. In the shader: `vec3 c = vec3(gl_FragCoord.xy/32.0, uSlice);` remap texel centers
   to 0..1 (`(c*32-0.5)/31`), apply grading, output.
4. In the composite: `graded = texture(uLUT, c*31.0/32.0 + 0.5/32.0).rgb;`
5. D3D11: a `Texture3D` with RTV per slice (`D3D11_RTV_DIMENSION_TEXTURE3D`,
   `FirstWSlice = z, WSize = 1`), or write it from a compute shader as a
   `RWTexture3D<float4>` in one dispatch of 32×32×32 threads.

Files: `src/modules/post/post_module.cpp` (buildLut, runLdr), `shaders/post/lut_gen.frag`, `shaders/post/postldr.frag`
