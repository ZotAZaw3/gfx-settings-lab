# Reflection and Light Probe Quality

Covers: environment/reflection probe resolution, prefilter sample count, probe update rate, parallax correction, and irradiance probe grid density / SH order.

## Definition
Probes are pre-captured views of the surroundings used for reflections (cubemaps) and indirect diffuse light (spherical harmonics). Their quality setting controls how sharp reflections look on glossy surfaces, how often they update when lighting changes (time of day), and how finely indirect light varies across the level.

| Value | Cube res | Prefilter samples | Update | Parallax | GI grid | SH |
|---|---|---|---|---|---|---|
| Low | 128 | 32 | on load | off | sky only | L1 |
| Medium | 256 | 64 | on change | on | 4×2×4 | L1 |
| High | 512 | 128 | on change, 1 face/frame | on | 8×4×8 | L2 |
| Ultra | 1024 | 256 | every frame (sky) | on | 12×6×12 | L2 |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Reflection Probe Resolution | `lighting.refl.probeResolution` | 128-1024 |
| Probe Prefilter Samples | `lighting.refl.prefilterSamples` | 32-256 |
| Reflection Probes | `lighting.refl.probes` | Sky only / Local / Local realtime |
| Light Probe Density | `lighting.gi.probeDensity` | 4x2x4 / 8x4x8 / 12x6x12 |
| Global Illumination | `lighting.gi.technique` | L1 vs L2 SH |

## How it works
- **Resolution** sets the sharpness of mirror reflections (mip 0). Rough surfaces use low mips, so resolution only matters for shiny materials.
- **Prefilter samples** reduce noise in the blurred mips; filtered importance sampling reads lower source mips for low-probability samples to stop fireflies.
- **Time slicing**: updating one face (or one mip) per frame spreads a 6-render cost over 6 frames.
- **Parallax correction** projects the reflection ray onto a box around the probe so nearby walls line up.
- **GI grid**: more probes = indirect light that follows walls and doorways more closely; L2 SH (9 coeffs) captures directional variation that L1 (4) blurs away.

## Breakdown
| Item | Cost |
|---|---|
| Capture | 6 scene renders per probe |
| Prefilter | mips × faces × res² × samples |
| SH projection | 2048 taps per probe (compute) |
| Memory | cube: res² × 6 × 8 B × 1.33; grid: probes × 7 × 8 B |

## Optimization
Bake offline and stream BC6H; relight probes from a stored G-buffer instead of re-rendering geometry; update only probes near the camera.

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Reflection Probe Resolution | 128 | -2.7 | -0.4 | -30.9 | -16.0 |
| Reflection Probe Resolution | 256 | -0.7 | +1.2 | -24.7 | -15.9 |
| Reflection Probe Resolution | 512 | -10.2 | -10.2 | +0.0 | +16.1 |
| Reflection Probe Resolution | 1024 | +1.2 | +1.2 | +99.0 | +144.0 |
| Probe Prefilter Samples | 32 | +2.2 | +3.1 | +0.0 | +16.0 |
| Probe Prefilter Samples | 64 | -5.9 | -6.6 | +0.0 | +32.0 |
| Probe Prefilter Samples | 128 | -4.2 | -2.8 | +0.0 | +32.0 |
| Probe Prefilter Samples | 256 | -5.6 | -0.0 | +0.0 | +32.0 |
| Reflection Probes | Sky only | +4.7 | +8.1 | +0.0 | +0.0 |
| Reflection Probes | Local probe | -5.6 | -2.9 | +0.0 | +0.0 |
| Reflection Probes | Local probe (realtime) | +95.4 | +96.5 | +0.0 | +0.0 |
| Light Probe Density | 4x2x4 | -4.8 | -6.6 | -0.0 | +0.0 |
| Light Probe Density | 8x4x8 | -5.0 | -5.8 | +0.0 | +0.0 |
| Light Probe Density | 12x6x12 | -7.7 | -7.9 | +0.1 | +0.0 |
| Global Illumination | Flat ambient | -24.1 | -27.2 | -0.0 | +0.0 |
| Global Illumination | Hemisphere + IBL | -22.7 | -24.5 | -0.0 | +0.0 |
| Global Illumination | Light probes (L1 SH) | -3.9 | -4.8 | +0.0 | +0.0 |
| Global Illumination | Light probes (L2 SH) | -4.4 | -6.8 | +0.0 | +0.0 |

## Rebuild (from scratch)
1. Capture: 6 × 90° renders to a cubemap; generate mips.
2. Prefilter per mip with GGX importance sampling.
3. For GI: per probe capture a small cube, project to SH in compute, write into 3D textures; sample trilinearly in shading.
4. Expose res/samples/update/grid/SH order in settings; reallocate on change.
