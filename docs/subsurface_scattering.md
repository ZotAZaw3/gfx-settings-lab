# Subsurface Scattering (SSS)

Covers: screen-space separable SSS (skin, wax, marble, leaves), wrap/pre-integrated diffuse fallback, thin-object transmission.
Code: `src/modules/lighting/SSSModule.cpp`, `shaders/lighting/sss_blur.comp`, `shaders/lighting/shade.glsl` (wrap + transmittance).

## Definition

In translucent materials light enters the surface, scatters inside, and exits a short distance away. The result: soft terminator lines, red bleeding at shadow edges on skin, glowing ears backlit by the sun, the waxy look of a candle. Plain Lambert shading treats light as entering and leaving at the same point, which makes skin look like plastic.

| Setting value | Technique |
|---|---|
| Off | Lambert |
| Low | wrap lighting (cheap soft terminator) |
| Medium | separable SSS, 11 taps |
| High | separable SSS, 17 taps, + transmittance |
| Ultra | separable SSS, 25 taps, + transmittance |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Subsurface Scattering | `lighting.sss.mode` | Off / Wrap / Screen-space 11 / 17 / 25 taps |
| SSS Transmittance | `lighting.sss.transmittance` | on/off |
| Scatter Width | `lighting.sss.width` | 0.001-0.1 m |
| SSS Strength | `lighting.sss.strength` | 0-1 |

## How it works

### Diffusion profile
How far light travels inside depends on wavelength: red goes deepest in skin, blue the least. The **diffusion profile** `R(r)` gives exiting light as a function of distance `r` from the entry point, per color channel. It is usually fitted as a sum of Gaussians (d'Eon & Luebke) or Burley's normalized diffusion `R(r) = (e^(−r/d) + e^(−r/3d)) / (8πdr)`.

### Screen-space separable SSS (Jimenez)
1. During lighting, write **diffuse** lighting of SSS materials to a separate buffer (specular stays sharp; it reflects at the surface).
2. Mark SSS pixels in the stencil buffer (or G-buffer material ID).
3. Blur the diffuse buffer horizontally, then vertically, with a kernel shaped like the profile. The kernel width in pixels is scaled by `sssWidth / depth` so it is constant in world units, and taps are rejected or weighted down when depth differs (no bleeding across silhouettes).
4. Add specular back on top.
The separable approximation is not exact (a 2D profile is not separable), but with 1D sums of Gaussians it is visually very close and ~10× cheaper than a 2D kernel.

### Transmittance (backlighting)
For thin parts (ears, nostrils, leaves), estimate thickness from the shadow map: distance between the depth stored in the light's map and the pixel's depth from the light. Light through the object = `profile(thickness) × lightColor × saturate(0.3 + dot(−N, L))`.

### Wrap / pre-integrated fallback
Low quality: `diffuse = saturate((N·L + w)/(1 + w))` with a tinted terminator. Pre-integrated skin uses a 2D LUT indexed by `N·L` and curvature.

## Breakdown

| Stage | Inputs | Output | Cost driver |
|---|---|---|---|
| Lighting split | lit scene | diffuse buffer (SSS pixels), specular buffer | MRT write |
| Stencil mark | material ID | stencil bit | free |
| Blur H | diffuse, depth, stencil | temp | taps × SSS-pixel count |
| Blur V | temp, depth | blurred diffuse | taps × SSS-pixel count |
| Transmittance | shadow map, depth | added light | 1 extra shadow tap |
| Composite | blurred + specular | HDR color | free |

Artifacts:
- **Halo / bleed** onto background → depth-aware tap weighting, stencil test.
- **Over-blurred close-ups** → the width must scale with depth (world-space).
- **Banding** at small taps → jitter kernel rotation and let TAA clean it.

## Optimization

- **Stencil test** so only SSS pixels run the blur (characters are a small fraction of the screen).
- **Precompute the kernel** on CPU per profile; upload once as a uniform array of `(rgb weight, offset)`.
- **Half res** for distant characters; skip entirely when the SSS object is < N pixels tall (LOD).
- **Separable** passes: 2·K taps instead of K².
- **Share the transmittance thickness** with the regular shadow lookup (same texel fetch).
- **Checkerboard** the blur and let TAA reconstruct (Unreal's approach).

### Measured cost

Mesa llvmpipe (software rasterizer, LLVM 20.1.2), 960×540, High preset baseline, generated with `gfxlab --bench-settings`. A frame takes about 250 ms on this CPU renderer and run-to-run noise is around ±10 ms, so read only large GPU deltas and the VRAM column as real; rerun the sweep on a real GPU for absolute numbers. llvmpipe keeps textures in system memory, so the RAM column mirrors VRAM.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| Subsurface Scattering | Off | -9.3 | -8.5 | -7.9 | +0.1 |
| Subsurface Scattering | Wrap lighting | +0.8 | -0.4 | -7.9 | +0.1 |
| Subsurface Scattering | Screen-space (11 taps) | +2.2 | +2.4 | +0.0 | +0.1 |
| Subsurface Scattering | Screen-space (17 taps) | -2.0 | -1.2 | +0.0 | +0.1 |
| Subsurface Scattering | Screen-space (25 taps) | +3.2 | +2.5 | +0.0 | +0.1 |
| SSS Transmittance | Off | -5.1 | -3.9 | +0.0 | +0.0 |
| SSS Transmittance | On | -3.0 | -3.1 | +0.0 | +0.0 |
| Scatter Width | 0.001 m | -6.6 | -5.1 | +0.0 | +0.0 |
| Scatter Width | 0.012 m | -4.0 | -3.2 | +0.0 | +0.0 |
| Scatter Width | 0.100 m | -2.2 | -2.9 | +0.0 | +0.0 |
| SSS Strength | 0.00 | -5.3 | -3.0 | +0.0 | +0.0 |
| SSS Strength | 1.00 | +3.9 | +6.4 | +0.0 | +0.0 |
| SSS Strength | 1.00 | +9.8 | +10.5 | +0.0 | +0.0 |

## Rebuild (from scratch)

1. Add a material ID / SSS flag and per-material `sssStrength`, `sssColor`/profile to the G-buffer.
2. In the lighting pass for SSS pixels, output diffuse to `RT_diffuse` and specular to `RT_spec` (MRT), others write normally.
3. CPU: build a K-tap kernel from a sum-of-Gaussians profile: offsets `o_i ∈ [−range, range]` distributed as `sign·|x|²`, weights = profile at `|o_i|`, normalized per channel.
4. Blur pass shader: `scale = sssWidth * projScale / linearDepth(uv)`; for each tap `uv_i = uv + dir * o_i * scale`; `w = kernel[i].rgb`; if `|depth_i − depth| > maxDelta`, lerp tap toward center color.
5. Run H then V with ping-pong targets; stencil-test both.
6. Composite: `final = blurredDiffuse + spec + transmittance`.
7. Transmittance: in lighting, `d = |shadowMapDepth − lightSpaceDepth| * scale`, `T = profile(d) * light * saturate(0.3 − dot(N, L))`.
8. Settings: `sssQuality` → technique, taps, resolution, transmittance on/off.
