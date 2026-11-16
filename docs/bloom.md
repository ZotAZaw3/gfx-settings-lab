# Bloom

> Menu: **Post Processing → Bloom** · Setting id(s): `post.bloom`, `post.bloom.intensity/threshold/knee/radius` · Owner module: `post`
> Values: On/Off; mips 4/5/6/7 from `post.quality` · Presets: Low=Off, Medium/High/Ultra=On

## Definition

Bloom is the soft glow that bleeds out of very bright areas: the halo around a
lamp, the sun smearing over a building edge, glowing neon. Real lenses and eyes
are not perfect; a small fraction of the light hitting each point scatters to
its neighbors. With normal brightness that scatter is invisible, but around
values far above white it becomes visible glow. In an HDR renderer bloom is
what *communicates* that something is brighter than the screen can display.

## How it works

The modern technique (Jimenez, "Next Generation Post Processing in Call of
Duty: Advanced Warfare", SIGGRAPH 2014; used in UE4/Unity) is a mip-chain blur:

1. **Prefilter / threshold** (full → 1/2 res). Keep only energy above the
   threshold using a *soft knee* so there is no hard edge:
   `contribution = max(softCurve(brightness), brightness - threshold) / brightness`.
   Modern physically based versions set threshold to 0 and use a low
   intensity (≈0.04), so *everything* blooms a tiny bit, like a real lens.
   The prefilter also applies a "Karis average" (weight each sample by
   `1/(1+luma)`) on the first downsample to kill *fireflies*, single very
   bright pixels that otherwise flicker as big blobs.
2. **Downsample chain** (1/2 → 1/4 → … → 1/64). Each step uses a 13-tap
   filter (4 overlapping 2×2 boxes + center) which is stable and does not
   alias as the camera moves.
3. **Upsample chain** (1/64 → … → 1/2). Each step upsamples the smaller mip
   with a 9-tap tent filter and *adds* it to the next larger mip. Because each
   mip is a progressively wider blur, the sum is a very wide, smooth,
   approximately exponential falloff ("long tail") at a fraction of the cost of
   one huge Gaussian. The *radius* setting controls how much each level spreads.
4. **Composite**: `scene + bloom * intensity`, *before* tonemapping, plus an
   optional lens-dirt texture multiplying the bloom (smudges lit up by bright
   light).

## Breakdown

| Pass | Shader | Target |
|---|---|---|
| Prefilter + first down | `bloom_down.frag` with `uPrefilter=1` | mip 0 (1/2 res) |
| Down ×(N−1) | `bloom_down.frag` | mips 1..N−1 |
| Up ×(N−1) | `bloom_up.frag`, additive blend `glBlendFunc(GL_ONE, GL_ONE)` | mips N−2..0 |
| Composite | in the PostOutput composite pass | output HDR |

Format: `R11F_G11F_B10F` (no alpha needed, half the bandwidth of RGBA16F).

## Optimization

- Total cost ≈ 1.33 × one half-res pass (geometric series), ~0.2–0.4 ms at
  1440p on mid-range GPUs.
- Fewer mips (Low preset: 4, High: 7) trade width for speed.
- Using bilinear taps placed between texels halves fetch count.
- A compute shader can do the whole downsample chain in one dispatch
  (AMD FidelityFX SPD), removing per-pass overhead.
- Karis average only on the first downsample; it darkens otherwise.

Measured cost at 1920×1080 (`gfxlab --bench-settings`, post settings only, High preset baseline, 60-frame windows), GPU: **Mesa llvmpipe (LLVM 20.1.2, 256 bits), a CPU software rasterizer in the cloud container**. A frame takes ~250 ms there, so treat these as relative costs only and expect about ±8 ms of run-to-run noise: anything smaller than that is "free". Δ VRAM is 0 because the module allocates its targets at startup/resize regardless of the toggle. Re-run on a real GPU for absolute numbers.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| `post.bloom` | Off | -7.9 | -8.5 | +0.0 | n/a |
| `post.bloom` | On | -0.7 | -4.2 | +0.0 | n/a |

## Rebuild (from scratch)

1. Allocate N textures, sizes `w>>(i+1), h>>(i+1)`, `GL_R11F_G11F_B10F`,
   `GL_LINEAR`, `GL_CLAMP_TO_EDGE`.
2. Downsample shader (13 taps, `t = 1/srcSize`):
   ```glsl
   vec3 a=S(-2,-2),b=S(0,-2),c=S(2,-2),d=S(-1,-1),e=S(1,-1),f=S(-2,0),g=S(0,0),
        h=S(2,0),i=S(-1,1),j=S(1,1),k=S(-2,2),l=S(0,2),m=S(2,2);
   vec3 o = e*0.125 + (a+c+k+m)*0.03125 + (b+f+h+l)*0.0625 + (d+e+i+j)*0.125;
   ```
   (`S(x,y) = texture(src, uv + vec2(x,y)*t).rgb`; the weights sum to 1.)
3. Upsample shader (3×3 tent): weights 1-2-1 / 2-4-2 / 1-2-1 over 16, scaled by
   radius; draw into the larger mip with additive blending enabled.
4. Final: `color += texture(bloomMip0, uv).rgb * intensity;`
5. D3D11: identical; use a blend state with `SrcBlend = DestBlend = D3D11_BLEND_ONE`
   for the upsample passes.

Files: `src/modules/post/post_module.cpp (runBloomDown`, `src/modules/post/runBloomUp`, `src/modules/post/runComposite)`, `shaders/post/bloom_down.frag`, `shaders/post/bloom_up.frag`, `shaders/post/composite_output.frag`
