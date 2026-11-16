# Vignette

> Menu: **Post Processing → Lens & Screen → Vignette** · Setting id(s): `post.vignette`, `post.vignette.intensity/smoothness/roundness` · Owner module: `post`
> Values: On/Off + sliders · Presets: On at every preset

## Definition

Vignetting is the darkening of an image toward its corners. In real lenses it
comes from the lens barrel blocking oblique light and from the `cos⁴θ` falloff
of light reaching the sensor at an angle. In games it focuses the eye on the
center of the screen and adds a cinematic feel; it is also used as a gameplay
signal (red vignette when hurt).

## How it works

1. Take the pixel's UV, center it: `d = uv − 0.5`.
2. Correct for aspect ratio so the vignette is a circle (roundness = 1) or
   follows the screen rectangle (roundness = 0):
   `d.x *= mix(1, aspect, roundness)`.
3. Compute the falloff: `v = smoothstep(r0, r1, length(d))`, where the radii
   come from intensity and smoothness. A physical alternative is
   `cos⁴(atan(length(d)·k))`.
4. Darken (or tint toward a color): `color = mix(color, vignetteColor, v * intensity)`.

It runs in the **PostLDR** stage, after tonemapping, so it darkens predictably
regardless of scene brightness. (A physically based vignette applied in HDR
before the tonemapper is the alternative; it affects bloom too.)

## Breakdown

| Piece | Where |
|---|---|
| Math | `postldr.frag` (`applyVignette`) |
| Uniforms | intensity, smoothness, roundness, color |

## Optimization

- About 8 ALU ops, fused into the PostLDR uber pass; effectively free.
- Could be baked into a tiny texture, but ALU is cheaper than a fetch today.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.vignette` | Off | +7.2 | +5.7 | +0.0 | n/a |
| `post.vignette` | On | +3.1 | +3.0 | +0.0 | n/a |

## Rebuild (from scratch)

```glsl
vec2 d = (vUV - 0.5);
d.x *= mix(1.0, uAspect, uRoundness);
float r = length(d) * 1.41421;                // 0 center, 1 corner
float v = smoothstep(1.0 - uSmoothness, 1.0 + uSmoothness*0.5, r + uIntensity*0.5);
color = mix(color, uVignetteColor, v * uIntensity);
```
D3D11: identical with `lerp`.

Files: `src/modules/post/post_module.cpp` (runLdr), `shaders/post/postldr.frag` (applyVignette)
