# Sun Glare & Lens Flare

> Menu: **Post Processing → Lens Flare & Sun Glare** · Setting id(s): `post.flare`, `post.flare.intensity/ghosts/spacing/halo/ca`, `post.glare`, `post.glare.intensity/streaks/size/anamorphic` · Owner module: `post`
> Values: On/Off each · Presets: Flare: Low/Medium=Off, High/Ultra=On · Glare: Low=Off, else On

## Definition

**Lens flare** is the set of artifacts a real camera lens produces when a
strong light shines into it: light reflects between the glass elements and
lands on the sensor as a chain of colored discs ("ghosts") along a line
through the screen center, a ring ("halo"), and streaks. **Sun glare** is the
starburst/glow directly around the sun (diffraction around the aperture blades
plus scattering in the eye/lens), which makes looking toward the sun feel
blinding. Anamorphic streaks are the long horizontal lines from cinema
anamorphic lenses.

## How it works

Two families of techniques exist:

1. **Sprite flares** (classic): project the sun to screen space, test its
   visibility (occlusion query or depth samples), draw textured sprites at
   positions `sunPos + t·(center − sunPos)`. Precise and art-directable, but
   only works for known lights.
2. **Screen-space flares** (Chapman, "Pseudo Lens Flare", 2013; used here):
   works for *any* bright pixel, including reflections and explosions.
   - Take the bloom prefiltered half-res image (only bright parts).
   - **Ghosts:** flip the UV around the center (`uv' = 1 − uv`), then sample
     along the vector to the center at `ghostCount` evenly spaced offsets.
     Weight each by a falloff from the center so ghosts fade near edges.
   - **Halo:** sample at a fixed distance `haloWidth` along the same vector,
     weighted by a ring-shaped mask.
   - **Chromatic distortion:** sample R, G, B at slightly different offsets
     along the direction, giving the rainbow fringe of real ghosts.
   - Blur the result (reuse a bloom mip) and multiply by a lens-dirt texture
     plus a starburst texture rotated with the camera.
3. **Sun glare / starburst**: the sun's screen position (from FrameConstants)
   and visibility (fraction of depth samples around it that are sky:
   `depth == 0` in reversed-Z) drive a procedural starburst:
   `glare = pow(max(0,1-dist/r), k) * (1 + streaks(angle))` where `streaks`
   is `pow(abs(cos(angle*N/2)), sharpness)` for N blades.

## Breakdown

| Pass | Shader | Res |
|---|---|---|
| Sun visibility | part of `flare.frag` (16 depth taps around sun) | — |
| Ghosts + halo + CA | `flare.frag` | 1/2, reads bloom mip 0 prefiltered |
| Blur | reuses the bloom upsample tent | 1/2 |
| Starburst + composite | in the PostOutput composite pass | full |

Stage: **PostOutput**, after bloom (it reuses bloom's thresholded image).

## Optimization

- Run at half or quarter resolution, flares are low-frequency.
- Sun visibility with 16 taps instead of an occlusion query avoids a GPU→CPU
  sync; result fades smoothly at partial occlusion.
- Skip the pass entirely when the sun is behind the camera and no pixel
  exceeds the threshold (cheap check via the eye-adaptation max).
- Ghost count 4–8; more is rarely noticeable.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.flare` | Off | -19.2 | -14.3 | +0.0 | n/a |
| `post.flare` | On | +18.0 | +11.8 | +0.0 | n/a |
| `post.glare` | Off | -5.0 | -5.5 | +0.0 | n/a |
| `post.glare` | On | -5.4 | -2.9 | +0.0 | n/a |

## Rebuild (from scratch)

1. Bright pass: `max(c - threshold, 0)` at half res (or reuse bloom prefilter).
2. Ghost shader:
   ```glsl
   vec2 uv = 1.0 - vUV;                       // flip
   vec2 dir = (vec2(0.5) - uv) * uSpacing;
   vec3 acc = vec3(0);
   for (int i=0;i<uGhosts;i++){
     vec2 o = fract(uv + dir*float(i));
     float w = pow(1.0 - length(vec2(0.5)-o)/0.707, 10.0);
     acc += chromaSample(uBright, o, normalize(dir)*uCA) * w;
   }
   vec2 h = normalize(dir) * uHaloWidth;
   float hw = pow(1.0 - length(vec2(0.5)-fract(uv+h))/0.707, 5.0);
   acc += chromaSample(uBright, fract(uv+h), normalize(dir)*uCA) * hw;
   ```
3. Blur, then in the composite add `flare * dirt * intensity` to HDR color.
4. Starburst: compute `d = uv - sunUV` (aspect corrected), `a = atan(d.y,d.x)`,
   add `sunColor * visibility * glare(d, a)`.
5. D3D11: identical; use `frac` and `atan2(y,x)` in HLSL.

Files: `src/modules/post/post_module.cpp` (runFlare, runComposite), `shaders/post/flare.frag`, `shaders/post/composite_output.frag`
