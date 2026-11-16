# Capsule (Character) Shadows

## Definition
Soft, analytic shadows cast by characters approximated as a few capsules (spine, limbs). Used under characters where shadow maps are low resolution or absent (indoor indirect light, distant cascades) so feet stay grounded. Unreal calls this "Capsule Direct/Indirect Shadows"; The Last of Us used the same idea.

| Value | Effect |
|---|---|
| Off | none |
| On | up to 8 capsules shadowing the sun term with a soft cone |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Capsule Shadows | `lighting.shadows.capsule` | on/off |

## How it works
For each pixel, for each capsule (segment `a–b`, radius `r`): find the closest point on the segment to the ray from the pixel toward the light, measure the distance `d` from that ray. If `d < r` the capsule blocks the light. The penumbra widens with distance `t` along the ray, using the light's cone angle, giving a contact-hardening soft shadow for a few ALU ops. No textures, no extra pass.

## Breakdown
`shadow_sample.glsl: capsuleShadow()` multiplies the sun visibility. In games capsules come from the animated skeleton each frame. This prototype has no skeletons, so `ShadowModule::buildCapsules` turns the 8 nearest character-sized instances (0.8 to 2.5 m tall, under 1 m wide) into vertical capsules and uploads them in the lighting UBO.

## Optimization
- Cull capsules per tile (like lights) when there are many characters.
- Apply to the indirect term too, using the dominant direction from the probe SH, for "indirect capsule shadows".
- Skip when the character is inside the high-res cascade and shadow quality ≥ High.

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Capsule Shadows | Off | +3.0 | +3.6 | +0.0 | +0.0 |
| Capsule Shadows | On | -2.0 | -4.3 | +0.0 | +0.0 |

## Rebuild (from scratch)
1. Fit capsules to bones (offline): radius and endpoints per bone.
2. Upload world-space endpoints each frame.
3. Implement segment-ray closest point, cone-based soft falloff, multiply into visibility.
