# Motion Blur

> Menu: **Post Processing → Motion Blur** · Setting id(s): `post.mb`, `post.mb.shutter/max/cameraOnly` · Owner module: `post`
> Values: On/Off; samples 6/8/12/16 from `post.quality` · Presets: Off by default

## Definition

A camera's shutter is open for a fraction of each frame. Anything moving
during that time is smeared along its path. Games add this to make low frame
rates feel smoother and to sell speed. **Camera motion blur** comes only from
camera movement/rotation; **per-object motion blur** also blurs things moving
independently (a spinning wheel, a passing car) while the camera stays still.

## How it works

1. **Velocity buffer.** The geometry pass writes, for every pixel, how far it
   moved on screen since last frame:
   `velocity = currClip.xy/currClip.w − prevClip.xy/prevClip.w` (NDC units, the
   engine's `scene.velocity` convention; multiply by `vec2(0.5, -0.5)` for UV units). `prevClip` uses last frame's view-projection *and* last frame's
   model matrix, so moving objects get their own velocity. This buffer is
   shared with TAA/DLSS/FSR, which also need it.
2. **Camera-only fallback** (no velocity buffer): reconstruct the world
   position from depth, reproject it with `prevViewProj`, and take the
   difference.
3. **Scale by shutter.** `v *= shutterAngle/360 * (targetFPS / currentFPS)`
   optional; clamp length to max blur.
4. **Tile max + neighbor max** (McGuire et al., "A Reconstruction Filter for
   Plausible Motion Blur", 2012). Find the dominant velocity in each 20×20
   tile, then the max over neighboring tiles. This lets fast objects blur
   *outward* past their silhouette into static background.
5. **Gather along the velocity.** For each pixel take N samples along the
   tile's dominant velocity (centered, ±0.5), with depth- and velocity-aware
   weights so background does not smear over a sharp foreground. A per-pixel
   random jitter hides banding between samples (cleaned up by TAA).

## Breakdown

| Pass | Shader | Notes |
|---|---|---|
| Velocity | written by the core geometry pass (`scene.velocity`, RG16F) | shared with TAA |
| Tile max | `mb_tilemax.frag` | 1/20 res |
| Neighbor max | `mb_neighbormax.frag` | 3×3 max of tiles |
| Gather | `mb_gather.frag` | full res, 8/12/16 samples by preset |

Stage: **PostRender** (render resolution, velocity matches color).

## Optimization

- Early-out pixels whose neighbor-max velocity is under 0.5 px (most of a
  still scene): the gather costs nothing there.
- 8 samples + jitter + TAA looks like 32.
- Tile passes are tiny (1/400 the pixels).
- Many players dislike motion blur: keep it off by default on high-refresh
  displays; at 144 Hz the real shutter blur from the display is enough.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.mb` | Off | -2.7 | -0.4 | +0.0 | n/a |
| `post.mb` | On | +15.4 | +17.6 | +0.0 | n/a |

## Rebuild (from scratch)

1. Velocity in the geometry pass:
   ```glsl
   // vertex: vCurr = uViewProj*uModel*pos; vPrev = uPrevViewProj*uPrevModel*pos;
   vec2 v = vCurr.xy/vCurr.w - vPrev.xy/vPrev.w;         // NDC delta (engine convention)
   outVelocity = v;
   ```
   (Remove TAA jitter from both before subtracting.)
2. Gather (simple version without tiles):
   ```glsl
   vec2 v = texture(uVelocity, uv).xy * vec2(0.5, -0.5) * uScale;   // NDC -> UV
   vec3 acc = vec3(0);
   float j = hash(gl_FragCoord.xy) - 0.5;
   for (int i=0;i<N;i++){ float t = (i + 0.5 + j)/N - 0.5;
     acc += texture(uColor, uv + v*t).rgb; }
   out = acc / N;
   ```
3. Add tile max/neighbor max when you see sharp silhouettes on fast objects.
4. D3D11: same; velocity format `DXGI_FORMAT_R16G16_FLOAT`.

Files: `src/modules/post/post_module.cpp` (runMotion), `shaders/post/mb_tilemax.frag`, `shaders/post/mb_neighbormax.frag`, `shaders/post/mb_gather.frag`
