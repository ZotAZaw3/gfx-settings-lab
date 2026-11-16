# Shadow Resolution, Shadow Distance and Update Rate

Covers the sub-settings that sit under "Shadow quality" in most games: shadow map resolution, shadow draw distance, cascade count, far-cascade update rate, and local-light (spot/point) shadow budget.
Code: `src/modules/lighting/ShadowModule.cpp`, `shaders/lighting/shadow_sample.glsl`.

## Definition
- **Resolution**: texels per side of each shadow map. More texels = sharper, less aliased shadows.
- **Distance**: how far from the camera shadows are drawn. Beyond it everything is lit.
- **Cascades**: how many slices the distance is split into (see shadows.md).
- **Update rate**: how often far cascades are re-rendered.

| Value | Resolution | Distance | Cascades | Far update |
|---|---|---|---|---|
| Low | 1024 | 40 m | 1 | every frame |
| Medium | 1024 | 80 m | 3 | every 2nd frame |
| High | 2048 | 150 m | 4 | every 2nd frame |
| Ultra | 4096 | 300 m | 4 | every frame |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Shadow Map Resolution | `lighting.shadows.resolution` | 512 / 1024 / 2048 / 4096 |
| Shadow Distance | `lighting.shadows.distance` | 10-500 m |
| Shadow Cascades | `lighting.shadows.cascades` | 1-4 |
| Far Cascade Update Rate | `lighting.shadows.farUpdate` | every 1 / 2 / 4 frames |

## How it works
Texel density on screen ≈ `resolution / (cascade world width)`. Doubling distance with the same resolution halves density for the far cascade, so distance and resolution trade against each other. The practical split scheme (λ = 0.75) spends most texels near the camera. Past the shadow distance a 10% fade avoids a hard line. Skipping updates on far cascades is invisible because their texels are large and the camera moves little per frame relative to them; the cascade matrix is kept from the frame it was rendered so the lookup stays consistent.

## Breakdown
| Cost | Scales with |
|---|---|
| Memory | cascades × res² × 4 bytes (4×4096² = 256 MB) |
| Render | draw calls in each cascade's box × cascades / update interval |
| Lookup | constant (1 cascade selected per pixel, 2 in blend band) |

## Optimization
- Cap distance aggressively; most of the image is near the camera.
- Use `DEPTH_COMPONENT16` for cascades ≥ 2.
- Distance-based object culling per cascade (small objects skip far cascades entirely).
- Bake static far shadows (distant shadow maps / shadow cache).

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Shadow Map Resolution | 512 | -7.2 | -6.4 | -60.0 | +0.0 |
| Shadow Map Resolution | 1024 | -3.7 | -2.7 | -48.0 | +0.0 |
| Shadow Map Resolution | 2048 | -1.6 | -2.1 | +0.0 | +64.0 |
| Shadow Map Resolution | 4096 | -7.0 | -6.2 | +192.0 | +256.0 |
| Shadow Distance | 10 m | -10.5 | -9.3 | +0.0 | +0.0 |
| Shadow Distance | 150 m | -6.5 | -4.5 | +0.0 | +0.0 |
| Shadow Distance | 500 m | -1.2 | -1.9 | +0.0 | +0.0 |
| Shadow Cascades | 1 | -6.1 | -7.0 | -48.0 | -64.0 |
| Shadow Cascades | 2 | -1.5 | -0.2 | -32.0 | -32.0 |
| Shadow Cascades | 3 | -2.5 | -3.3 | -16.0 | -16.0 |
| Shadow Cascades | 4 | +6.2 | +8.0 | +0.0 | +0.0 |
| Far Cascade Update Rate | Every frame | +7.9 | +8.3 | +0.0 | +0.0 |
| Far Cascade Update Rate | Every 2nd frame | -6.1 | -6.7 | +0.0 | +0.0 |
| Far Cascade Update Rate | Every 4th frame | -10.1 | -9.9 | +0.0 | +0.0 |

## Rebuild (from scratch)
1. Make resolution and cascade count parameters of the shadow array allocation; reallocate on change.
2. Clamp the split far plane to `shadowDistance`, not camera far.
3. Add `frameIndex % interval` check per cascade before rendering; keep per-cascade matrices.
4. Fade: `mix(1, V, saturate((dist − viewDepth) / (0.1·dist)))`.
