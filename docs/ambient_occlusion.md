# Ambient Occlusion

Covers: SSAO (Crytek/normal-oriented hemisphere), HBAO / HBAO+ style horizon-based AO, GTAO (ground-truth AO).
Code: `src/modules/lighting/AOModule.cpp`, `shaders/lighting/ssao.comp`, `shaders/lighting/hbao.comp`, `shaders/lighting/gtao.comp`, `shaders/lighting/ao_blur.comp`.

## Definition

Ambient occlusion estimates how much of the sky/ambient light each point can "see". Creases, corners, and the gap under a sofa receive less indirect light than an open floor. AO is a scalar `A ∈ [0,1]` that multiplies the **indirect** (ambient, IBL, GI) lighting only; direct light already has shadows.

Formally `A(p) = 1/π ∫Ω V(p,ω) (n·ω) dω`: the cosine-weighted fraction of the hemisphere that is unoccluded.

| Setting value | Technique |
|---|---|
| Off | `A = 1` |
| Low | SSAO, 8 samples, half resolution, 4×4 blur |
| Medium | SSAO, 16 samples, full res, bilateral blur |
| High | HBAO, 8 directions × 4 steps, full res, bilateral blur |
| Ultra | GTAO, 4 slices × 8 steps, cosine-weighted, temporal accumulation + bilateral blur |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Ambient Occlusion | `lighting.ao.mode` | Off / SSAO / HBAO / GTAO |
| AO Quality | `lighting.ao.quality` | Low (half res) / Medium / High / Ultra |
| AO Radius | `lighting.ao.radius` | 0.05-3 m |
| AO Intensity | `lighting.ao.intensity` | 0-4, exponent (`ao^intensity`), default 1.5 |
| AO Multi-Bounce | `lighting.ao.multibounce` | on/off |

## How it works

All three run in screen space from the depth buffer (and normal buffer) as a fullscreen pass.

### SSAO
1. Reconstruct the view-space position `P` and normal `N` of the pixel.
2. Build a hemisphere kernel of K random points oriented around `N` (a TBN with a random rotation from a 4×4 noise texture to avoid banding).
3. For each sample `S = P + TBN·k_i · radius`, project it to screen, read the depth there. If the scene surface is in front of `S`, that sample is occluded.
4. Range check: ignore occluders farther than `radius` from `P` (fixes dark halos around silhouettes).
5. `A = 1 − occluded / K`, then blur with the 4×4 noise period.

### HBAO (horizon-based)
Instead of random points, treat the depth buffer as a heightfield. For each of D directions in screen space, march outwards and track the highest elevation angle (the *horizon*) seen relative to the tangent plane. Occlusion in that direction is `sin(h) − sin(t)` (horizon angle minus tangent angle), attenuated by distance. Average over directions. Fewer samples than SSAO for a smoother, more physically meaningful result.

### GTAO (ground-truth)
GTAO solves the AO integral analytically per *slice*. For each slice direction φ:
1. March both ways along the slice to find the two horizon angles `h1`, `h2`.
2. Project the normal onto the slice plane, get angle `n` and length.
3. Integrate the cosine-weighted visible arc in closed form:
   `a = ¼·(−cos(2h1 − n) + cos n + 2h1·sin n) + ¼·(−cos(2h2 − n) + cos n + 2h2·sin n)`
4. Weight by projected normal length, average slices.
With temporal rotation of slice angles and TAA, 2–4 slices per pixel per frame converge to near ray-traced AO. GTAO also offers a *multi-bounce* fit that brightens AO on light albedos.

## Breakdown

| Stage | Inputs | Output | Cost driver |
|---|---|---|---|
| Depth (linear) | depth buffer | R32F linear depth (optional mip chain) | resolution |
| AO kernel | linear depth, normals, noise | R8 raw AO | samples × resolution |
| Bilateral blur (H+V) | raw AO, depth | R8 AO | taps × resolution |
| Apply | AO, lighting | `indirect *= A` (and specular occlusion) | free |

Common problems:
- **Halos** around foreground objects → range check / distance falloff.
- **Banding** → random rotation per pixel + blur.
- **Noise crawling** → blur must be depth-aware (bilateral) so edges stay sharp.
- **Over-darkening flat walls** in SSAO → use normal-oriented hemisphere, not full sphere, and a small depth bias.

## Optimization

- **Half resolution** + depth-aware upsample: ¼ the cost, nearly invisible behind blur.
- **Hierarchical depth (HiZ mips)**: far samples read coarser mips → cache-friendly; key for HBAO/GTAO with large radii.
- **Interleaved / deinterleaved rendering** (HBAO+ trick): split the depth buffer into 4×4 quarter-res layers so neighboring threads read neighboring texels.
- **Temporal accumulation**: rotate the noise every frame and let TAA (AA thread) or a small history buffer average it, so you can halve sample counts.
- **Separable bilateral blur**: 2 passes of 1D instead of one 2D kernel.
- **Bent normals** (GTAO): free by-product, improves diffuse IBL direction.
- **Store AO in G-buffer alpha** when a baked AO texture exists; combine `min(bakedAO, ssao)`.

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Ambient Occlusion | Off | -31.0 | -34.5 | -2.0 | +0.3 |
| Ambient Occlusion | SSAO | -9.1 | -10.8 | +0.0 | +0.3 |
| Ambient Occlusion | HBAO | -2.3 | -5.7 | +0.0 | +0.3 |
| Ambient Occlusion | GTAO | -6.8 | -6.9 | +0.0 | +0.3 |
| AO Quality | Low | -17.9 | -15.9 | -1.5 | +0.0 |
| AO Quality | Medium | +4.6 | +5.0 | +0.0 | +0.0 |
| AO Quality | High | +3.3 | +3.8 | +0.0 | +0.0 |
| AO Quality | Ultra | +2.8 | +3.7 | +0.0 | +0.0 |
| AO Radius | 0.05 m | +8.7 | +9.5 | +0.0 | +0.0 |
| AO Radius | 0.60 m | +15.0 | +16.5 | +0.0 | +0.0 |
| AO Radius | 3.00 m | +13.6 | +14.5 | +0.0 | +0.0 |
| AO Intensity | 0.00 | +2.9 | +0.3 | +0.0 | +0.0 |
| AO Intensity | 1.50 | +7.9 | +9.8 | +0.0 | +0.0 |
| AO Intensity | 4.00 | +14.7 | +15.4 | +0.0 | +0.0 |
| AO Multi-Bounce | Off | -7.8 | -6.9 | +0.0 | +0.0 |
| AO Multi-Bounce | On | +4.8 | +5.7 | +0.0 | +0.0 |

## Rebuild (from scratch)

1. After the G-buffer pass, have depth (`sampler2D`) and view-space normals available.
2. Generate a 4×4 RGB16F noise texture of random unit vectors in the XY plane, `GL_REPEAT`.
3. Generate a kernel of K samples in the +Z hemisphere: random `(x,y ∈ [−1,1], z ∈ [0,1])`, normalize, scale by `lerp(0.1, 1, (i/K)²)` so samples cluster near the center.
4. Fullscreen fragment shader: `P = reconstructViewPos(uv, depth)`, `N = normal`, `rand = texture(noise, uv * screenSize / 4)`, Gram-Schmidt TBN, loop K samples, project, compare with `+bias`, range-check with `smoothstep(0,1, radius / |P.z − sceneZ|)`.
5. Output to an R8 target. Blur with a 4×4 box (or bilateral: weight = gaussian × exp(−|Δdepth|·k)).
6. HBAO: replace step 4's loop with D directions `φ_i = 2π(i + rand)/D`, march S steps of `radius/S` in screen pixels, track `maxSinH`.
7. GTAO: replace with slices; implement the closed-form integral above. Rotate the slice angle by the frame index × golden ratio for temporal stability.
8. In the lighting/composite pass: `ambient *= ao; specularIBL *= specularOcclusion(NdotV, ao, roughness)`.
9. Settings: `aoQuality` → technique, sample count, resolution scale, radius, intensity.
