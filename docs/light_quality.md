# Light Count and Light Quality

Covers: max dynamic lights, tiled light culling, light falloff/range, area-light approximation, BRDF quality.
Code: `shaders/lighting/deferred_lighting.comp` (tiled culling), `shaders/lighting/shade.glsl` (light loop), `src/modules/lighting/LightingModule.cpp`.

## Definition
"Light quality" or "Local lights" in a settings menu controls how many dynamic lights (lamps, muzzle flashes, neon signs) are evaluated per pixel and how accurately. Low settings drop distant or small lights or merge them into ambient; high settings shade every light with a full physically based BRDF and soft area-light highlights.

| Value | Max lights | Culling | BRDF | Area approx |
|---|---|---|---|---|
| Low | 4 | per pixel loop | normalized Blinn-Phong | no |
| Medium | 16 | per pixel loop | GGX | no |
| High | 64 | 16×16 tiled compute | GGX + multi-scatter | no |
| Ultra | 256 | 16×16 tiled compute | GGX + multi-scatter | representative point |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Light Quality | `lighting.lights.quality` | Low / Medium / High / Ultra |
| Max Dynamic Lights | `lighting.lights.max` | 0-256 |

## How it works
- Each light has a **range**. Falloff is inverse-square, multiplied by a window `saturate(1 − (d/R)⁴)²` so it reaches exactly zero at `R`. Finite range is what makes culling possible.
- **Light cap**: lights are sorted on the CPU by screen importance (`intensity / distance²`) and only the first N are uploaded.
- **Tiled culling**: one compute workgroup per 16×16 pixel tile finds the tile's min/max depth, builds a view-space bounding box, tests every light's sphere against it, and stores the survivors in shared memory. Pixels then loop only over their tile's list, so 256 lights cost roughly what 10–20 cost untiled.
- **Area approximation**: real lamps are not points. Widening the GGX roughness by `sourceRadius / (2·distance)` (Karis) makes highlights on glossy surfaces the right size instead of a tiny pinprick.

## Breakdown
| Stage | Cost driver |
|---|---|
| CPU sort + upload (SSBO) | lights in scene |
| Tile depth bounds | 2 shared atomics per pixel |
| Tile culling | lights / 256 per thread |
| Shading | lights per tile × pixels |

## Optimization
- Clustered (tiles × depth slices) beats tiled when depth ranges are large (foliage against sky).
- Fade lights out by distance instead of popping; fold faded lights into the ambient probe.
- Shadow-casting lights are the expensive ones: cap them separately (usually 1 sun + 2–4 local).
- Use `half` math in the loop on mobile.
- Toggle "Debug: light heatmap" to see per-tile counts (tile list length → color).

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Light Quality | Low | -3.8 | -6.9 | +0.0 | +0.0 |
| Light Quality | Medium | -6.4 | -6.4 | +0.0 | +0.0 |
| Light Quality | High | -7.9 | -9.3 | +0.0 | +0.0 |
| Light Quality | Ultra | -3.9 | -4.8 | +0.0 | +0.0 |
| Max Dynamic Lights | 0 | -10.4 | -10.1 | +0.0 | +0.0 |
| Max Dynamic Lights | 64 | +2.0 | +3.5 | +0.0 | +0.0 |
| Max Dynamic Lights | 128 | +4.6 | +4.4 | +0.0 | +0.0 |
| Max Dynamic Lights | 192 | +8.2 | +7.7 | +0.0 | +0.0 |
| Max Dynamic Lights | 256 | +9.7 | +10.1 | +0.0 | +0.0 |

## Rebuild (from scratch)
1. Define a `PunctualLight` struct (pos, range, color, type, spot cone, radius), std430 SSBO.
2. Per pixel loop with the windowed falloff; validate with 1 light.
3. Add the compute tiled version: shared `minZ/maxZ` atomics (float bits as uint are order-preserving for positive floats), corner rays → AABB, sphere-AABB test, `atomicAdd` into a shared list.
4. Swap the loop's light source with a macro so the same shading function works for both paths.
5. Expose `Light quality` → max lights, tiled on/off, BRDF model, area approx.
