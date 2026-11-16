# Panini Projection (Wide-FOV Correction)

> Menu: **Post Processing → Lens & Screen → Panini projection** · Setting id(s): `post.panini`, `post.panini.distance/crop` · Owner module: `post`
> Values: On/Off, distance 0…1 · Presets: Off by default

## Definition

A normal (rectilinear) perspective projection keeps straight lines straight,
but at wide fields of view (100°+, common on ultrawide monitors and in fast
shooters) objects near the edges get stretched enormously. The **Panini
projection** is a cylindrical-style projection that keeps vertical lines
straight and radial lines through the center straight, while greatly reducing
edge stretching. Used in UE4/5, Unity HDRP and some shooters.

## How it works

1. The scene is rendered normally with a wide rectilinear FOV.
2. A post pass remaps each output pixel: compute the direction it should
   represent under Panini projection, then find where that direction lands in
   the rectilinear image and sample there.
3. Panini math with distance parameter `d` (Sharpless et al., 2010): for a
   view-space point at horizontal angle φ, the Panini x-coordinate is
   `S·sin φ` and y is `S·tan θ` with `S = (d+1)/(d+cos φ)`. The inverse
   (screen → direction) has a closed form, used in the shader.
4. **Crop to fit** scales the result so no empty border appears at the edges.

Stage: **PostOutput**, before bloom (so bloom is not distorted twice).
(Ideally before the upscaler too, but it's an output-space effect in most
engines.)

## Breakdown

| Piece | Where |
|---|---|
| Remap | `panini.frag` |
| Inputs | horizontal FOV from `FrameConstants.proj`, distance, crop |

## Optimization

One pass, one fetch (bilinear). The UV remap can be precomputed into a
texture, but ALU is cheaper. Slight softening at edges: render at a slightly
higher resolution or let TAA/upscaling compensate.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.panini` | Off | +0.5 | -0.1 | +0.0 | n/a |
| `post.panini` | On | +14.9 | +14.9 | +0.0 | n/a |

## Rebuild (from scratch)

```glsl
// p: output pixel in [-1,1] scaled by half-extent of the projected view plane
vec2 paniniInverse(vec2 p, float d){
  float k = p.x*p.x/((d+1.0)*(d+1.0));
  float dscr = k*k*d*d - (k+1.0)*(k*d*d-1.0);
  float clon = (-k*d + sqrt(dscr))/(k+1.0);
  float S = (d+1.0)/(d+clon);
  float lon = atan(p.x, S*clon);
  float lat = atan(p.y, S);
  return vec2(sin(lon), tan(lat)) / cos(lon);   // rectilinear view-plane coords
}
```
Map output UV → view plane (`(uv*2-1)*halfExtent`), call `paniniInverse`,
map back to UV (`/halfExtent*0.5+0.5`) and sample.

Files: `src/modules/post/post_module.cpp` (runPanini), `shaders/post/panini.frag`
