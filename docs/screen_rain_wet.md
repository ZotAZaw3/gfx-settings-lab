# Screen-Space Rain Drops & Wet Lens

> Menu: **Post Processing → Lens & Screen → Rain on lens** · Setting id(s): `post.rain`, `post.rain.amount/size/fog` · Owner module: `post`
> Values: On/Off; drop layers 1/2/3/3 from `post.quality` · Presets: Off by default

(Wet *surfaces*, meaning darker albedo, higher specular and puddle
reflections, are a material/lighting effect and belong to the reflections
module; this file covers what happens on the camera "lens".)

## Definition

Rain drops landing on the camera lens or a helmet visor refract the scene
behind them, act as tiny lenses, slide down and leave trails. Racing games and
third-person shooters use it to sell weather and immersion.

## How it works

1. **Drop field.** The screen is divided into a grid of cells; each cell holds
   at most one drop whose position, size and lifetime come from a hash of the
   cell id. Several grid layers at different scales give small and big drops.
   Drops in the "streak" layer move downward over time with a wobbling x, and
   leave a trail of tiny static droplets behind.
2. **Normal from the drop shape.** Inside a drop, treat it as a hemisphere:
   the vector from the drop center gives a pseudo surface normal.
3. **Refraction.** Offset the scene UV by the normal (`uv + n.xy * strength`),
   and sample a *blurred* scene (a bloom mip) for the area outside drops to
   imitate the fogged, out-of-focus lens; sharp sample inside drops.
   A real drop flips the image; a small negative offset imitates that.
4. **Highlight.** Add a tiny specular spot at the drop's upper-left to catch
   light.

Stage: **PostOutput** (it needs HDR so highlights inside drops still bloom).

## Breakdown

| Piece | Where |
|---|---|
| Drop layers | `rain_lens.frag` (2 static + 1 streak layer) |
| Blurred background | reads bloom mip 1 |
| Time | `FrameConstants.time` |

## Optimization

Procedural, no textures: ~60 ALU per layer, < 0.15 ms at 1440p. Lower presets
use fewer layers. Skip when rain amount is 0.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.rain` | Off | -6.9 | -6.7 | +0.0 | n/a |
| `post.rain` | On | +14.9 | +16.9 | +0.0 | n/a |

## Rebuild (from scratch)

1. For each layer: `vec2 g = uv*vec2(aspect,1)*scale; vec2 id=floor(g); vec2 f=fract(g)-0.5;`
2. Random per cell: `vec3 r = hash3(id);` drop center `c = (r.xy-0.5)*0.7`,
   radius `rad = mix(0.1,0.3,r.z)`; mask `m = smoothstep(rad, rad*0.8, length(f-c))`.
3. Normal `n = (f-c)/rad`; accumulate `offset += n*m`.
4. `color = mix(blurred(uv), sharp(uv - offset*k), maxMask)`.
5. D3D11: identical.

Files: `src/modules/post/post_module.cpp` (runRain), `shaders/post/rain_lens.frag`
