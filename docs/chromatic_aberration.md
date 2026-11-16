# Chromatic Aberration

> Menu: **Post Processing → Lens & Screen → Chromatic aberration** · Setting id(s): `post.ca`, `post.ca.intensity` · Owner module: `post`
> Values: On/Off; spectral taps 3/6/10/16 from `post.quality` · Presets: Off by default

## Definition

A glass lens bends different wavelengths by slightly different amounts
(dispersion), so red, green and blue do not land on exactly the same spot.
**Lateral (transverse) chromatic aberration** shows as colored fringes (red/cyan
or purple/green) along high-contrast edges, growing toward the screen edges.
Games use it for a "cheap camera" look, for damage/drugged effects, or in VR
to *cancel* the headset lens's aberration.

## How it works

1. Direction from the screen center: `d = uv − 0.5`. Aberration grows with
   distance from the center (`|d|²`).
2. Sample the image three times along `d`: red scaled outward, blue inward,
   green in the middle:
   `r = tex(uv + d·k).r; g = tex(uv).g; b = tex(uv − d·k).b`.
3. **Spectral version (quality setting):** take N samples along `d` and weight
   each by a spectrum-to-RGB ramp (red→green→blue) instead of 3 hard channels.
   This gives smooth rainbow fringes with no visible triple image.
4. Runs in **PostLDR**, after tonemapping, so fringes don't create extra
   bloom; it reads the tonemapped image via `ctx.post.src()`.

## Breakdown

| Piece | Where |
|---|---|
| Sampling | `postldr.frag` (`chromaticAberration`) |
| Samples | 3 (Low), 6 (Medium), 10 (High) spectral taps |

## Optimization

- 3 fetches per pixel is cheap; spectral up to 10 is still < 0.1 ms at 1440p.
- Skip entirely when intensity is 0 (shader branch on a uniform is free: all
  pixels take the same path).
- Fused into the PostLDR uber pass so the image is only read once more.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.ca` | Off | -3.4 | -0.4 | +0.0 | n/a |
| `post.ca` | On | +37.5 | +38.0 | +0.0 | n/a |

## Rebuild (from scratch)

```glsl
vec2 d = vUV - 0.5;
vec2 off = d * dot(d,d) * uIntensity;
vec3 sum = vec3(0), wsum = vec3(0);
for (int i=0;i<N;i++){
  float t = float(i)/float(N-1);              // 0..1 across the spectrum
  vec3 w = clamp(vec3(1.0-abs(t*2.0-0.0)*1.0, 1.0-abs(t*2.0-1.0), 1.0-abs(t*2.0-2.0)), 0.0, 1.0);
  sum  += texture(uSrc, vUV - off*(t-0.5)*2.0).rgb * w;
  wsum += w;
}
color = sum / wsum;
```
D3D11: identical.

Files: `src/modules/post/post_module.cpp` (runLdr), `shaders/post/postldr.frag` (chromaticAberration)
