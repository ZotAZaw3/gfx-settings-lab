# Sharpening (AMD CAS, NVIDIA NIS, unsharp mask)

> Menu: **Anti-Aliasing & Upscaling → Sharpening, Sharpness** · Setting id(s): `aa.sharpen.mode`, `aa.sharpen.strength`, `aa.sharpen.split` (debug) · Owner module: `aa.post`
> Values: Off / CAS / RCAS / NIS-lite / Unsharp mask; 0-1 · Presets: Low=Off, Medium..Ultra=CAS

Settings shown in the menu: `Sharpening: Off | CAS | NIS | Unsharp`, `Sharpness 0.0 - 1.0`

## Definition

Sharpening increases local contrast around edges and texture detail to counteract the softness introduced by TAA, upscaling, FXAA and mip filtering. **CAS** (Contrast Adaptive Sharpening, AMD FidelityFX, 2019) is the most common: it sharpens a lot where the image is flat-ish and little where contrast is already high, so it avoids the halos and noise of a naive unsharp mask. **NIS** (NVIDIA Image Scaling) is a 6-tap directional scaler + sharpener that can also upscale, and is what the driver-level "NVIDIA Image Scaling" toggle runs.

## How it works

**Unsharp mask (baseline):** `out = c + k * (c - blur(c))`. Simple; produces halos and amplifies noise.

**CAS:**
1. Read the pixel and its 4 neighbours (cross: N, S, E, W), optionally the 4 diagonals.
2. Find per-channel min and max of the neighbourhood.
3. Compute how much headroom is left before clipping: `amp = sqrt(saturate(min(mn, 1 - mx) / mx))`. Low contrast → amp near 1, high contrast (already near 0 or 1) → amp near 0.
4. Build a negative-lobe weight: `w = amp * mix(-1/8, -1/5, sharpness)`.
5. Output `(c + w*(n+s+e+w)) / (1 + 4w)`: a sharpening kernel whose strength adapts per pixel.
6. CAS can also resample to a different output size ("CAS upscaling"), but it is weak at that compared to FSR.

**NIS:** a directional edge detector chooses between 4 orientations and weights a 6x6 filter (precomputed coefficient textures) along the edge, then adds adaptive sharpening. It is spatial-only (like FSR 1).

## Breakdown

| Method | Taps | Halo control | Can upscale | Cost 1080p |
|---|---|---|---|---|
| Unsharp mask | 5-9 | none | no | ~0.05 ms |
| CAS | 5 or 9 | adaptive | weak | ~0.08 ms |
| NIS | 36 (6x6) | adaptive | yes, ~1.3-2x | ~0.2 ms |
| RCAS (FSR's CAS) | 5 | adaptive + noise detect | no | ~0.06 ms |

Placement in this engine: stage `PostLDR`, after tone mapping, before film grain/UI. Sharpening in HDR overshoots on bright pixels.

Code: `src/modules/aa/post_aa.cpp`, `shaders/aa/cas.frag`, `shaders/aa/nis_lite.frag` (simplified NIS: directional 4-tap edge-aware filter, not the coefficient-table version), `shaders/aa/unsharp.frag`, `shaders/aa/rcas.frag` (FSR's robust sharpener).

## Optimization

- 5-tap CAS is enough after TAA; 9-tap only for heavy blur.
- Fuse sharpening into the tone-map pass when nothing sits between them (saves one full-screen read/write).
- Auto-scale sharpness with render scale: more when upscaling from 50%, less at native.
- Disable on UI and text.

### Measured quality

Measured with `tests/aa/quality_test.py` (512x288 procedural scene: thin lines, a moving rotated square, HDR highlights; Mesa llvmpipe). PSNR against a 16x16 supersampled reference, higher is better.

Applied after FXAA, strength 0.5 (PSNR vs reference, so it rewards sharpness only when it's correct detail):

| Sharpener | PSNR |
|---|---|
| FXAA alone | 37.4 dB |
| + RCAS | 37.2 dB |
| + NIS-lite | 37.3 dB |
| + CAS | 35.2 dB |
| + Unsharp mask | 32.3 dB |

Unsharp mask overshoots (halos) the most; the adaptive sharpeners stay close to the reference.

## Rebuild (from scratch)

```glsl
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 a = texelFetch(uTex, p + ivec2( 0,-1), 0).rgb;
    vec3 b = texelFetch(uTex, p + ivec2(-1, 0), 0).rgb;
    vec3 c = texelFetch(uTex, p,                0).rgb;
    vec3 d = texelFetch(uTex, p + ivec2( 1, 0), 0).rgb;
    vec3 e = texelFetch(uTex, p + ivec2( 0, 1), 0).rgb;
    vec3 mn = min(min(min(a, b), min(c, d)), e);
    vec3 mx = max(max(max(a, b), max(c, d)), e);
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-5), 0.0, 1.0));
    vec3 w = amp * mix(-0.125, -0.2, uSharpness);
    o = vec4(clamp((c + w * (a + b + d + e)) / (1.0 + 4.0 * w), 0.0, 1.0), 1);
}
```

Steps:
1. Run after tone map on LDR colour.
2. Expose sharpness, show a split-screen compare (left off, right on) in the debug menu.
3. Check halos on a dark object against a bright sky; CAS should show far less than unsharp at equal perceived sharpness.
