# Lens Dirt

> Menu: **Post Processing → Bloom → Lens dirt** · Setting id(s): `post.bloom.dirt` · Owner module: `post`
> Values: 0…2 · Presets: Low=0, Medium=0, High=0.4, Ultra=0.4

## Definition

Dust, smudges and fingerprints on the front of a camera lens are invisible in
normal light but light up when a bright source shines through them, showing
as soft blotches and scratches over the bloom. Battlefield 3 popularized it;
it sells "this is a camera" and makes bright lights feel intense.

## How it works

1. A full-screen **dirt texture** (grayscale or tinted smudges, 1920×1080 or
   procedurally generated) is stretched over the screen.
2. In the composite, the *bloom* (and flare) contribution is multiplied by it:
   `hdr += bloom * (bloomIntensity + dirt(uv) * dirtIntensity)`.
   Because bloom is near zero where nothing is bright, the dirt is invisible
   until a light source passes behind it, exactly like a real lens.
3. Using the widest bloom mip (instead of mip 0) to drive the dirt makes the
   whole smudge pattern glow when a light is anywhere nearby, matching how a
   real lens is lit by off-axis light.

## Breakdown

| Piece | Where |
|---|---|
| Procedural dirt | `dirt_gen.frag`, generated once into an R8 texture: layered value noise blobs + a few streaks |
| Apply | PostOutput composite (`uDirtIntensity`) |

## Optimization

One extra texture fetch in an existing pass; generated once, ~2 MB at R8 1080p.
Use a half-res texture with linear filtering, smudges are blurry anyway.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.bloom.dirt` | 0.00 | -1.4 | +0.3 | +0.0 | n/a |
| `post.bloom.dirt` | 0.40 | +6.1 | +6.3 | +0.0 | n/a |
| `post.bloom.dirt` | 2.00 | +9.8 | +9.1 | +0.0 | n/a |

## Rebuild (from scratch)

1. Load or generate a dirt image (`GL_R8`, linear, clamp).
2. Composite: `vec3 b = texture(uBloom, uv).rgb; hdr += b*uBloomI + b*texture(uDirt, uv).r*uDirtI;`
3. D3D11: identical.

Files: `src/modules/post/post_module.cpp` (runComposite), `shaders/post/dirt_gen.frag`, `shaders/post/composite_output.frag`
