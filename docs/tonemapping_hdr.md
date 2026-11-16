# Tonemapping & HDR

> Menu: **Post Processing → Tonemapping** · Setting id(s): `post.tonemap`, `post.tonemap.white` · Owner module: `post`
> Values: None / Reinhard / Reinhard extended / Hable filmic / ACES / AgX · Presets: ACES on every preset

## Definition

The renderer computes light in physical-ish units, so a sky can be 20× brighter
than a white wall and the sun 1000× brighter. A normal (SDR) monitor can only
show 0..1. **Tonemapping** is the curve that compresses that unlimited range
into the displayable range while keeping contrast in the midtones and rolling
highlights off smoothly instead of clipping them to flat white. **HDR output**
means sending a wider range (e.g. up to 1000 nits in the PQ/Rec.2020 format) to
an HDR display, which still needs a (gentler) tonemap to the display's peak.

## How it works

The tonemapper is a function `f(x)` applied per pixel (per channel or on
luminance) after exposure:

| Operator | Formula (x = exposed linear color) | Character |
|---|---|---|
| Clamp | `min(x, 1)` | harsh clipping, hue shifts, reference only |
| Reinhard | `x / (1 + x)` | never clips, but flat and desaturated |
| Reinhard extended | `x(1 + x/W²) / (1 + x)` | reaches pure white at white point `W` |
| Hable (Uncharted 2) | `((x(Ax+CB)+DE)/(x(Ax+B)+DF)) − E/F`, normalized by `f(W)` | filmic toe and shoulder |
| ACES fitted (Narkowicz/Hill) | RRT+ODT fit in AP1 space | punchy film-like contrast, standard in UE4 era |
| AgX (Sobotka) | log2 encode → sigmoid polynomial → inverse matrix | best hue preservation for very bright saturated lights (Blender 4 default) |

After tonemapping the result is still *linear* 0..1. The engine core encodes
it with the sRGB transfer function (≈ `pow(x, 1/2.2)`, plus the user's
gamma/brightness) or PQ for HDR displays at the very end, after the PostLDR
stage (grading, grain, vignette).

Why per-channel curves desaturate: a bright pure red `(10, 0.5, 0.5)` gets its
red channel compressed far more than the others, so it shifts toward white.
That is "filmic" and often wanted (fire looks white-hot). AgX deliberately
controls this by compressing in a rotated color space.

## Breakdown

| Piece | Where |
|---|---|
| Operator functions + exposure | `shaders/post/tonemap.frag` (ToneMap stage) |
| Selection | `post.tonemap` → `PostUBO.pTonemap.x` |
| White point | `uWhitePoint`, used by Reinhard extended and Hable |
| sRGB / PQ encode | owned by the core output pass (exact piecewise sRGB, not 2.2) |
| HDR output path | when the core reports HDR output, the curve's white point becomes `peakNits/paperWhiteNits` and the output is scene-referred relative to paper white; the core converts 709→2020 and PQ-encodes (see `hdr_calibration.md`) |

## Optimization

- Tonemapping is ~20 ALU instructions; it is never the bottleneck. Fuse it
  into the final composite (done).
- **Bake into the LUT.** Many engines bake tonemap + grading into one 32³
  3D LUT sampled with a log-encoded HDR input, so the whole thing is one
  texture fetch. This prototype keeps them separate for clarity and so each
  can be toggled.
- Use `half` precision on mobile; ACES fit is stable in fp16.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.tonemap` | None (clamp) | +2.7 | +4.8 | +0.0 | n/a |
| `post.tonemap` | Reinhard | +2.1 | +2.0 | +0.0 | n/a |
| `post.tonemap` | Reinhard extended | +2.7 | +1.4 | +0.0 | n/a |
| `post.tonemap` | Hable filmic | -0.1 | +1.5 | +0.0 | n/a |
| `post.tonemap` | ACES | +2.6 | +1.6 | +0.0 | n/a |
| `post.tonemap` | AgX | +7.8 | +8.2 | +0.0 | n/a |

## Rebuild (from scratch)

1. Make sure your scene target is floating point (`RGBA16F`), otherwise there
   is no HDR to map.
2. In the last pass:
   ```glsl
   vec3 ACESFitted(vec3 c){
     const mat3 inM = mat3(0.59719,0.07600,0.02840, 0.35458,0.90834,0.13383, 0.04823,0.01566,0.83777);
     const mat3 outM= mat3(1.60475,-0.10208,-0.00327, -0.53108,1.10813,-0.07276, -0.07367,-0.00605,1.07602);
     c = inM * c;
     vec3 a = c*(c+0.0245786)-0.000090537;
     vec3 b = c*(0.983729*c+0.4329510)+0.238081;
     return clamp(outM * (a/b), 0.0, 1.0);
   }
   ```
3. Encode: `srgb = c <= 0.0031308 ? 12.92*c : 1.055*pow(c,1/2.4)-0.055`.
   (Or create the backbuffer as `GL_FRAMEBUFFER_SRGB` / `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`
   and let hardware encode; then *do not* encode in the shader.)
4. HDR10 output: create a 10-bit swap chain (`DXGI_FORMAT_R10G10B10A2_UNORM`,
   color space `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`), tonemap to the
   display's peak nits instead of 1.0, convert 709→2020, PQ-encode.

Files: `src/modules/post/post_module.cpp` (runTonemap), `shaders/post/tonemap.frag`
