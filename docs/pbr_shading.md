# PBR Shading and Lighting

Covers: physically based BRDF (Cook-Torrance GGX), metal/roughness workflow, punctual lights (directional, point, spot), image-based lighting, energy conservation, and the deferred lighting pass every other lighting module plugs into.
Code: `src/modules/lighting/LightingModule.cpp`, `shaders/lighting/pbr_common.glsl`, `shaders/lighting/deferred_lighting.comp`, `shaders/lighting/shade.glsl`.

## Definition

Physically based rendering shades surfaces with models that obey real optics: energy is conserved (a surface never reflects more light than it receives), all materials have Fresnel reflection, and roughness controls how sharp highlights are. The payoff is that one material looks right under any lighting, so artists stop tweaking per scene.

| Setting value | What changes |
|---|---|
| Low | Blinn-Phong-normalized specular, no multi-scatter, 4 point lights |
| Medium | GGX + Smith + Schlick, 16 lights |
| High | + specular occlusion, multi-scatter energy compensation, 64 lights (tiled) |
| Ultra | + area-light approximation (representative point), 256 lights (tiled/clustered) |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Light Quality | `lighting.lights.quality` | Low / Medium / High / Ultra |
| Clear Coat Layer | `lighting.pbr.clearcoat` | on/off |
| Sky / IBL Intensity | `lighting.ambient.intensity` | 0-4 |

## How it works

### Material inputs (metal/roughness)
- **Base color**: albedo for dielectrics, F0 tint for metals.
- **Metallic** 0/1: metals have no diffuse and colored specular.
- **Roughness** `r`; we use `α = r²` (perceptually linear).
- `F0 = mix(vec3(0.04), baseColor, metallic)`.

### Cook-Torrance BRDF
```
f = kd · albedo/π  +  D·G·F / (4 (N·L)(N·V))

D (GGX)    = α² / (π ((N·H)²(α²−1)+1)²)
G (Smith)  = G1(L)·G1(V), height-correlated form preferred
F (Schlick)= F0 + (1−F0)(1−V·H)^5
kd         = (1−F)(1−metallic)
```
Outgoing radiance per light: `Lo = f · radiance · (N·L) · shadow`.

### Light types
- **Directional** (sun): constant direction, CSM shadows.
- **Point**: inverse-square falloff with a windowed range `saturate(1 − (d/R)⁴)² / d²`.
- **Spot**: point + smooth cone `smoothstep(cosOuter, cosInner, dot(−L, spotDir))`.

### Image-based lighting
- Diffuse: irradiance from SH or an irradiance cubemap (see global_illumination.md).
- Specular: prefiltered cubemap + BRDF LUT (see reflections.md).
- Multi-scatter compensation (Fdez-Agüera): `E_ss = F0·A + B`, `E_ms = 1 − (A+B)`, adds energy lost by single-scatter GGX at high roughness.

### Deferred lighting pass
G-buffer pass writes per-pixel material; a fullscreen lighting pass loops lights, reads the shadow, AO, SSR, GI and SSS inputs produced by sibling modules, and writes HDR radiance. Post-processing (tone mapping, bloom) happens after, in the post thread.

## Breakdown

| Input | Source module | Used for |
|---|---|---|
| G-buffer albedo/normal/ORM/depth | core engine | BRDF inputs |
| Shadow visibility | ShadowModule | direct light × V |
| AO | AOModule | indirect diffuse × A, specular occlusion |
| Reflection buffer | ReflectionModule | indirect specular |
| Irradiance | GIModule | indirect diffuse |
| SSS split | SSSModule | diffuse goes to separate buffer for SSS pixels |

Output: `RGBA16F` HDR radiance.

## Optimization

- **Tiled / clustered lighting**: split the screen into 16×16 tiles (× depth slices), cull lights per tile in a compute shader, each pixel loops only its tile's lights.
- **Light range windowing** so lights have finite influence and cull cleanly.
- **Pack the G-buffer**: octahedral normals in RG16, roughness/metal/AO in one RGBA8.
- **Precompute** BRDF LUT and prefiltered IBL once.
- **Branch on metallic**: skip diffuse for full metals.
- **Half-precision math** (`mediump`) on mobile; most terms are fine in fp16.
- **Specular anti-aliasing** (Toksvig / Kaplanyan): widen roughness by normal variance to stop sparkling highlights; cheaper than more MSAA.

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Light Quality | Low | -3.8 | -6.9 | +0.0 | +0.0 |
| Light Quality | Medium | -6.4 | -6.4 | +0.0 | +0.0 |
| Light Quality | High | -7.9 | -9.3 | +0.0 | +0.0 |
| Light Quality | Ultra | -3.9 | -4.8 | +0.0 | +0.0 |
| Clear Coat Layer | Off | -9.0 | -9.6 | +0.0 | +0.0 |
| Clear Coat Layer | On | -3.9 | -3.3 | +0.0 | +0.0 |
| Sky / IBL Intensity | 0.00 | -5.7 | -6.4 | +0.0 | +0.0 |
| Sky / IBL Intensity | 1.00 | -0.7 | -0.4 | +0.0 | +0.0 |
| Sky / IBL Intensity | 4.00 | +21.2 | +15.2 | +0.0 | +0.0 |

## Rebuild (from scratch)

1. G-buffer: RT0 RGBA8 (baseColor, AO), RT1 RGB10A2 or RG16 (normal), RT2 RGBA8 (roughness, metallic, materialID, sss), depth D32F.
2. Write `pbr_common.glsl` with `D_GGX`, `V_SmithGGXCorrelated`, `F_Schlick`, `evaluateLight()`.
3. Fullscreen pass: reconstruct world position from depth and inverse view-projection. Loop lights, accumulate `Lo`.
4. Multiply sun term by shadow visibility from `ShadowModule`.
5. Add ambient: `kd·albedo·irradiance·AO + prefilteredSpec·(F0·A + B)·specOcc`, blended with SSR.
6. Write HDR. Hand off to the post stack for tone mapping and gamma.
7. Settings: `lightingQuality` → BRDF model, max lights, tiled culling, multi-scatter.
