# Specular Occlusion

## Definition
AO darkens diffuse ambient light, but reflections from the environment cubemap also should not appear inside crevices: a shiny sphere's underside should not reflect the bright sky. Specular occlusion derives a reflection-visibility factor from AO, view angle and roughness, so cubemap reflections fade in occluded areas without needing SSR.

| Value | Effect |
|---|---|
| Off | probe/IBL reflections everywhere (glowing creases) |
| On | Lagarde approximation applied to specular IBL |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Specular Occlusion | `lighting.ao.specularOcclusion` | on/off |

## How it works
`specOcc = saturate(pow(NoV + ao, exp2(−16·roughness − 1)) − 1 + ao)` (Lagarde & de Rousiers, Frostbite 2014). At grazing angles and low roughness the reflection cone is narrow and points away from the occluder, so occlusion is weaker; rough surfaces behave like diffuse and get roughly `ao`. GTAO's bent normal + cone intersection gives a more accurate version.

## Breakdown
One line in `shade.glsl` (`specularOcclusion()` in `pbr_common.glsl`), multiplied into the IBL specular term. Zero extra passes; needs the AO buffer.

## Optimization
Free. Use the bent-normal cone version only on Ultra.

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Specular Occlusion | Off | -11.5 | -10.8 | +0.0 | +0.0 |
| Specular Occlusion | On | -4.6 | -3.7 | +0.0 | +0.0 |

## Rebuild (from scratch)
1. Have AO (baked + SSAO/GTAO) available at shading.
2. Compute the factor above, multiply prefiltered-env specular by it (not direct specular, which has shadows).
