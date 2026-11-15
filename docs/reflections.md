# Reflections

Covers: cubemap / image-based reflections (IBL, reflection probes), planar reflections, screen-space reflections (SSR). Ray-traced reflections are owned by the ray tracing thread; this module provides the raster fallback chain they replace.
Code: `src/modules/lighting/ReflectionModule.cpp`, `shaders/lighting/{sky_to_cube,ibl_prefilter,brdf_lut,hiz_build,ssr}.comp`, `shaders/lighting/ss_trace.glsl`, `shaders/lighting/forward_lite.{vert,frag}` (planar + probe capture), planar composite in `deferred_lighting.comp`.

## Definition

A reflection is light that bounces off a surface toward the eye in the mirror direction `R = reflect(−V, N)`. Rough surfaces spread that bounce into a lobe, so reflections get blurrier as roughness rises. A rasterizer only knows what is directly visible, so reflection techniques are different ways to answer "what would I see along R?".

| Setting value | Chain used (first hit wins, falls back down the list) |
|---|---|
| Low | static skybox cubemap (prefiltered IBL) |
| Medium | + local reflection probes (parallax-corrected cubemaps) |
| High | + SSR, 32 steps, half res |
| Ultra | + SSR, 64 steps full res with HiZ tracing, + planar reflection for the ground plane, realtime local probe |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Screen Space Reflections | `lighting.refl.ssr` | Off / Low / Medium / High / Ultra (HiZ) |
| Reflection Probes | `lighting.refl.probes` | Sky only / Local probe / Local probe (realtime) |
| Planar Reflections | `lighting.refl.planar` | on/off |

## How it works

### Cubemap / IBL
1. Capture the environment into a cubemap (6 renders with 90° FOV from a probe position, or an HDR sky).
2. **Prefilter** it for roughness: mip 0 = mirror, mip k = convolved with the GGX lobe of roughness `k/(mips−1)` (split-sum approximation).
3. Precompute a 2D **BRDF LUT** (NdotV × roughness → scale, bias for F0).
4. At shading: `spec = textureLod(prefiltered, R, roughness·maxMip).rgb * (F0·lut.x + lut.y)`.

**Parallax correction** (local probes): a cubemap is captured at one point, so reflections of nearby walls look wrong elsewhere. Intersect `R` with the probe's box proxy and use the vector from the probe center to that hit point as the lookup direction.

### Planar reflections
For a flat mirror or water, render the scene a second time with the camera mirrored across the plane (`M = I − 2·n·nᵀ` in homogeneous form) and an oblique near clip plane so geometry below the surface is clipped. Sample that texture with the pixel's projected screen UV (plus a normal-map distortion). Exact, but costs a full extra scene render per plane.

### SSR
Ray-march `R` through the depth buffer of the current frame:
1. Start at the pixel's view position, step along `R` in view space (or better, in screen space so each step is ~1 pixel).
2. At each step project to screen, compare ray depth with depth buffer.
3. When the ray goes behind the surface within a thickness, binary-search to refine the hit, then read the previous frame's lit color at that UV.
4. Fade out where the ray leaves the screen, faces the camera, or hits nothing; fall back to probes there.
5. For rough surfaces, jitter `R` by a GGX importance sample and blur/denoise (or sample the color buffer's mip chain by cone width).

## Breakdown

| Stage | When | Output | Cost driver |
|---|---|---|---|
| Probe capture | load time / on demand | cubemap per probe | 6 scene renders per probe |
| Prefilter + LUT | load time | mips + RG16F LUT | one-off |
| Planar render | per frame per plane | color target | full scene draw |
| HiZ build | per frame | depth min-mip chain | resolution |
| SSR trace | per frame | RGBA hit color + confidence | steps × resolution |
| Resolve / blur | per frame | reflection buffer | taps |
| Composite | lighting | `lerp(probe, ssr, ssr.a)` × Fresnel | free |

Limits: SSR cannot show anything off-screen or behind objects (the classic "reflection disappears when you look down"). Probes are static and low frequency. Planar works only on flat surfaces. That is why games blend all three, and why ray-traced reflections exist.

## Optimization

- **Half-res SSR** + temporal reprojection; reflections are usually viewed at a glancing angle so softness hides resolution.
- **HiZ tracing**: march through the min-depth mip pyramid, taking big steps in empty space; ~4–10× fewer steps than linear marching.
- **Roughness cutoff**: skip SSR above roughness 0.6, use probes only.
- **Stochastic SSR** with ray reuse across neighbors (Frostbite "SSSR") gives glossy reflections from 1 ray/pixel.
- **Planar**: render at half res, with reduced LOD, no particles, no shadows update; cull to the reflected frustum.
- **Probes**: bake offline; store as BC6H; blend between 2 nearest probes only.
- **Fresnel early-out**: at normal incidence on dielectrics, F ≈ 0.04; low-confidence pixels can skip SSR.

### Measured cost

Pending: the per-value table (Δ GPU ms, Δ CPU ms, Δ VRAM, Δ RAM at 1920×1080) is generated by `gfxlab --bench-settings docs/_measurements` once the engine builds, and will be pasted here.

## Rebuild (from scratch)

1. Load an HDR equirect sky, render to a 512² cubemap (6 faces, 90° FOV).
2. Prefilter: for each mip `m` (roughness `m/(M−1)`), render each face with a shader that importance-samples GGX around the face direction (Hammersley sequence, 64–1024 samples).
3. BRDF LUT: 512² RG16F, fullscreen shader integrating `G_Smith·Vis` over GGX samples for each (NdotV, roughness).
4. In PBR lighting: add `specIBL = prefiltered(R, rough) * (F0·A + B)`.
5. SSR: fullscreen pass after lighting. View position from depth; `R = reflect(normalize(P), N)`; step 32–64 times in screen space using DDA, compare `rayZ` vs `sceneZ` with thickness; refine 4 binary steps; output `prevColor(uv)` and confidence `= edgeFade × (1 − roughness) × facingFade`.
6. Composite: `reflection = mix(probeSpec, ssrColor, confidence)`.
7. Planar: build mirrored view matrix, set oblique projection (Lengyel's method) to clip at the plane, render scene into a target, sample with projective UV in the water/mirror material.
8. Settings: `reflectionQuality` → enables, steps, resolution scale.
