# TAA (Temporal Anti-Aliasing)

> Menu: **Anti-Aliasing & Upscaling → Anti-Aliasing = TAA, TAA History Clip, TAA History Weight** · Setting id(s): `aa.method`, `aa.taa.clip`, `aa.taa.history` · Owner module: `aa.temporal`
> Values: Off / FXAA / SMAA / TAA; clip 0.5-3.0; history 0.5-0.98 · Presets: Low=FXAA, Medium=SMAA, High=TAA, Ultra=TAA

Setting shown in the menu: `Anti-Aliasing: TAA` (+ `TAA Sharpness`, `TAA History Weight`)

## Definition

TAA spreads supersampling across time. Every frame the camera is shifted by a different sub-pixel offset (jitter), and the new frame is blended with an accumulated history buffer that has been moved (reprojected) to line up with the current frame using motion vectors. Over ~8-16 frames each pixel effectively sees many sample positions, removing geometric, shader and temporal aliasing (shimmer). Its downsides are blur and ghosting, which most of the algorithm is there to fight. TAA is also the base algorithm of DLSS, FSR 2+, XeSS and TSR.

## How it works

Per frame:

1. **Jitter:** offset the projection matrix by a sub-pixel amount from a low-discrepancy sequence (Halton 2,3; 8 or 16 phases): `proj[2][0] += jx * 2 / width; proj[2][1] += jy * 2 / height`.
2. **Render** the scene (with velocity, see `motion_vectors.md`).
3. **Resolve pass** for each pixel:
   1. *Dilate velocity:* take the velocity of the closest-depth pixel in a 3x3 cross (keeps edges attached to the moving foreground).
   2. *Reproject:* `prevUV = uv - velocity`. Sample history at `prevUV` with a **Catmull-Rom** (bicubic) filter; bilinear would blur more every frame.
   3. *Neighbourhood clamp / clip:* compute min/max (or mean ± γ·stddev, "variance clipping") of the current frame's 3x3 neighbourhood in **YCoCg** space. Clip the history colour into that box. If the history disagrees with what's on screen now (disocclusion, lighting change), it gets pulled toward current colours instead of ghosting.
   4. *Blend:* `out = mix(history, current, alpha)`, alpha ≈ 0.1 (history weight 0.9). Raise alpha when velocity is high or when the history was clipped hard.
   5. *Anti-flicker:* weight by `1 / (1 + luma)` (tonemapped blending) so HDR highlights don't dominate.
4. Write `out` to the new history (ping-pong two textures) and to the screen.

## Breakdown

| Component | Purpose | Failure if missing |
|---|---|---|
| Jitter | gives new sample positions | no AA, only blur |
| Motion vectors | align history | smearing when moving |
| Velocity dilation | edge stability | ghost outline behind objects |
| Catmull-Rom history | keep sharpness | image gets blurrier over time |
| Variance clipping | reject stale history | ghosting, trails |
| YCoCg | tighter colour box | colour ghosting |
| Tonemap weighting | stable highlights | flicker on specular |
| Sharpening after | win back detail | soft image |

Cost: one full-screen pass, ~0.3-0.5 ms at 1080p, plus one extra RGBA16F history buffer.

Code: `src/modules/aa/temporal.cpp`, `shaders/aa/taa_resolve.frag`, `shaders/aa/common_temporal.glsl` (shared with the upscaler).

## Optimization

- Variance clipping (γ = 1.0-1.25) beats min/max clamping for ghosting vs flicker.
- Use the 5-tap Catmull-Rom approximation (5 bilinear taps instead of 16 point taps).
- Apply a mild negative mip bias (-0.5) to keep textures sharp under jitter.
- Put CAS sharpening after TAA.
- Exclude particles/transparents from velocity; render them after TAA or give them a reactive mask.
- On camera cuts, reset history (alpha = 1 for one frame).

### Measured cost

Measured cost at 1920×1080 (`gfxlab --bench-settings`), GPU: **Mesa llvmpipe 25.2.8 (software rasterizer, CPU)**. Deltas are against the High preset (TAA, CAS, AF 16x, native). On llvmpipe a whole frame takes hundreds of ms and the run-to-run noise is about ±8 ms (changing a setting with no rendering effect moves it that much), so only large differences mean anything; on a real GPU every pass here costs well under 1 ms. Greyed-out vendor options (DLSS, FSR 3, XeSS) snap back to our version, so they aren't listed.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Anti-Aliasing | Off | -22.2 | -21.9 | +0.0 | +0.2 |
| Anti-Aliasing | FXAA | +1.0 | -1.1 | +0.0 | +0.2 |
| Anti-Aliasing | SMAA | +6.0 | +6.0 | +0.0 | +0.3 |
| Anti-Aliasing | TAA | +0.9 | +0.7 | +0.0 | +0.3 |
| TAA History Clip | 0.50 | -1.9 | -1.2 | +0.0 | +0.0 |
| TAA History Clip | 1.25 | -0.2 | +1.7 | +0.0 | +0.0 |
| TAA History Clip | 3.00 | -0.7 | -0.5 | +0.0 | +0.0 |
| TAA History Weight | 0.50 | -6.9 | -6.4 | +0.0 | +0.0 |
| TAA History Weight | 0.92 | -5.6 | -4.1 | +0.0 | +0.0 |
| TAA History Weight | 0.98 | -7.5 | -7.4 | +0.0 | +0.0 |

### Measured quality

Measured with `tests/aa/quality_test.py` (512x288 procedural scene: thin lines, a moving rotated square, HDR highlights; Mesa llvmpipe). PSNR against a 16x16 supersampled reference, higher is better.

| Case | No AA | TAA (32 frames) |
|---|---|---|
| Static camera | 31.2 dB | 37.1 dB |
| Object moving 2 px/frame | 31.2 dB | 35.6 dB |

The drop in motion is the price of history rejection: newly revealed pixels start from one sample.

## Rebuild (from scratch)

```glsl
vec3 RGB2YCoCg(vec3 c) { return vec3(dot(c, vec3(.25,.5,.25)), dot(c, vec3(.5,0,-.5)), dot(c, vec3(-.25,.5,-.25))); }
vec3 YCoCg2RGB(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
void main() {
    vec2 px = 1.0 / vec2(textureSize(uCurrent, 0));
    // 1. closest-depth velocity
    vec2 best = vec2(0); float bd = 0.0;                // reversed-Z: closest = largest
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        float d = texture(uDepth, uv + vec2(x, y) * px).r;
        if (d > bd) { bd = d; best = vec2(x, y); }
    }
    vec2 vel = velocityToUV(texture(uVelocity, uv + best * px).rg); // NDC delta -> UV delta
    // 2. neighbourhood stats
    vec3 m1 = vec3(0), m2 = vec3(0), cur = vec3(0);
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        vec3 c = RGB2YCoCg(texture(uCurrent, uv + vec2(x, y) * px).rgb);
        if (x == 0 && y == 0) cur = c;
        m1 += c; m2 += c * c;
    }
    vec3 mu = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0));
    vec3 lo = mu - 1.25 * sigma, hi = mu + 1.25 * sigma;
    // 3. reproject + clip
    vec2 prevUV = uv - vel;
    vec3 hist = RGB2YCoCg(sampleCatmullRom(uHistory, prevUV));
    hist = clamp(hist, lo, hi);
    bool offscreen = any(lessThan(prevUV, vec2(0))) || any(greaterThan(prevUV, vec2(1)));
    float a = offscreen || uReset ? 1.0 : mix(0.08, 0.3, saturate(length(vel / px) / 8.0));
    o = vec4(YCoCg2RGB(mix(hist, cur, a)), 1);
}
```

Steps:
1. Add Halton jitter to the projection (and pass the unjittered one for velocity).
2. Allocate two RGBA16F history textures at output resolution; ping-pong.
3. Implement the resolve shader above.
4. Reset history on resize/camera cut/AA mode change.
5. Debug views: history only, clipping amount, velocity.
6. Test: rotate the camera past a fence and a thin pole; adjust γ and alpha until ghosting and shimmer are both acceptable.
