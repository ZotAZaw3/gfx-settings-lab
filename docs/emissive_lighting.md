# Emissive Lighting

Code: `shaders/lighting/shade.glsl` (`shadeSurface` adds `s.emissive * intensity`), `deferred_lighting.comp`, `forward_lite.frag` (probe capture), `src/modules/lighting/LightingModule.cpp`.

## Definition
Emissive materials glow on their own: screens, lava, neon, LEDs. Games expose an emissive or "self-illumination" quality that decides whether that glow only shows on the surface itself, or also lights the world around it.

In this app there is one direct control, **Emissive Intensity** (`lighting.emissive.intensity`, Lighting tab, not touched by presets), and the glow's reach comes from the GI settings:

| GI setting | What emissive does |
|---|---|
| any | drawn on the surface, bloom (post thread) adds the halo |
| Screen Space GI on | lights nearby on-screen geometry (SSGI reads last frame's lit color, emissive included) |
| Light probes (L1/L2) | baked into the probe grid, so the glow survives off-screen |

## How it works
- The G-buffer pass writes `emissive × intensity` straight into the HDR scene color; the lighting resolve adds on top. Bloom (post) creates the halo.
- SSGI reads last frame's lit color, which includes emissive, so emissive surfaces light nearby geometry automatically.
- (Not implemented here, common in games) "Emissive to light" spawns a punctual light at the emitter's center with color = average emissive and range ∝ √intensity, so the glow reaches off-screen surfaces.
- Probe baking renders the lit scene including emissive into each probe, so the glow persists in the GI grid.

## Breakdown
| Path | Cost |
|---|---|
| Surface only | free |
| SSGI | covered by SSGI cost |
| Promoted lights (extension) | counts toward the light budget (light_quality.md) |
| Probes | bake time only |

## Optimization
Promote only the brightest N emitters; animate intensity on the light, not by re-baking.

## Rebuild (from scratch)
1. Add an emissive texture/color and intensity (in nits or EV) to the material; write it to scene color in the G-buffer pass.
2. Keep lighting additive into that target.
3. Feed previous-frame color into SSGI.
4. Add a CPU pass that converts tagged emitters into lights each frame.
