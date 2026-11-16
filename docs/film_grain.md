# Film Grain

> Menu: **Post Processing → Lens & Screen → Film grain** · Setting id(s): `post.grain`, `post.grain.intensity/size/response/colored` · Owner module: `post`
> Values: On/Off + sliders · Presets: Off by default

## Definition

Film grain is the random, fine-scale noise visible in photographic film (from
silver halide crystals) or digital sensors (photon shot noise and read noise).
In games it adds texture, hides color banding in smooth gradients (sky, fog),
masks low-resolution or blurry textures, and gives a cinematic or horror look.

## How it works

1. **Noise per pixel per frame.** A hash of `(pixel coordinate, frame index)`
   produces a random value in −0.5..0.5. Changing it every frame makes grain
   "boil" like film; a static pattern would look like a dirty screen.
2. **Grain size**: compute the hash on `floor(pixel / size)` coordinates and
   smooth slightly, or sample a tiling pre-generated noise texture with
   scaled UVs (better looking, used here: procedural value noise).
3. **Luminance response**: real grain is most visible in midtones and less in
   highlights:  `amount = intensity * mix(1, 1 − luma, response)`.
4. **Apply** after tonemapping: `color += noise * amount` (mono) or with a
   separate noise per channel (colored). Adding noise *before* quantization to
   8 bits also acts as dithering and removes banding.

Stage: **PostLDR**, last effect before the core's output encode.

## Breakdown

| Piece | Where |
|---|---|
| Hash | `postldr.frag` (`hash12`, PCG-based integer hash, no `sin()` precision issues) |
| Frame index | `FrameConstants.frameIndex` |
| Response curve | `uGrainResponse` |

## Optimization

- An integer hash is ~10 ALU ops; free in the uber pass.
- Blue noise (texture) looks finer and less clumpy than white noise at the same
  intensity, at the cost of one texture fetch.
- Keep a small always-on dither (≈0.5/255) even when grain is off to
  prevent banding in skies; this module does that.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.grain` | Off | -5.3 | -4.9 | +0.0 | n/a |
| `post.grain` | On | -3.3 | -1.4 | +0.0 | n/a |

## Rebuild (from scratch)

```glsl
float hash(uvec3 v){ v = v*1664525u + 1013904223u;
  v.x += v.y*v.z; v.y += v.z*v.x; v.z += v.x*v.y; v ^= v>>16u;
  v.x += v.y*v.z; return float(v.x) / 4294967295.0; }
vec2 p = floor(gl_FragCoord.xy / uGrainSize);
float n = hash(uvec3(p, uFrame)) - 0.5;
float luma = dot(color, vec3(0.2126,0.7152,0.0722));
color += n * uGrain * mix(1.0, 1.0 - luma, uResponse);
```
D3D11: HLSL supports `uint3` the same way.

Files: `src/modules/post/post_module.cpp` (runLdr), `shaders/post/postldr.frag` (applyGrain)
