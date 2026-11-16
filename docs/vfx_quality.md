# VFX / Effects Quality

Menu: **Effects → Effects Quality** (`Low | Medium | High | Ultra`) — a *preset* that drives
several sub-settings at once; **Effects Resolution** (`Quarter | Half | Full`),
**Distortion / Heat Haze** (on/off), **Flipbook Interpolation** (on/off)
Code: `GeoVfxSettings::vfxQuality` → `applyVfxPreset()`; `vfxResolution`, `distortion`,
`flipbookBlend`; shaders `particles.frag`, `distortion.frag`

## Definition

"Effects quality" is a catch-all that covers every transient visual effect that is not
lighting or geometry: explosions, muzzle flashes, sparks, smoke, fire, magic spells,
heat haze, water splashes, footprints, screen-space distortion, trails/ribbons and
lens dirt. In most games it is a bundle: one slider that changes particle counts,
effect render resolution, sprite texture size, distortion, and how many effects may be
alive at once.

## How it works

The preset fans out like this in the prototype:

| Sub-setting               | Low      | Medium  | High    | Ultra   |
|---------------------------|----------|---------|---------|---------|
| Particle quality          | Low      | Medium  | High    | Ultra   |
| Effects render resolution | Quarter  | Half    | Full    | Full    |
| Heat haze / distortion    | off      | off     | on      | on      |
| Flipbook frame blending   | off      | on      | on      | on      |
| Max live emitters         | 4        | 8       | 16      | 32      |
| Spawn-rate scale          | 0.25     | 0.5     | 1.0     | 1.0     |

Key techniques:
- **Off-screen (reduced-res) effects buffer.** Transparent effects are drawn into a
  1/2 or 1/4 resolution colour target with a downsampled depth buffer, then composited
  with a **bilateral upsample** that picks the low-res sample whose depth best matches
  the full-res pixel. Avoids halos around edges.
- **Flipbooks.** Animated fire/smoke is an atlas of frames. With interpolation, the
  shader blends frame N and N+1 (or uses motion vectors baked per frame to warp between
  them), so 16 frames look as smooth as 64.
- **Distortion.** Heat haze / shockwaves render normal-map-like offsets into a
  distortion buffer, then a fullscreen pass samples the scene colour at
  `uv + offset`. Needs a copy of the scene colour (cannot read and write the same target).
- **Emitter budget.** A manager keeps the N most important effects (closest, biggest,
  gameplay-relevant) and culls the rest.

## Breakdown

- Effects resolution: Full → Half is ~4× fewer blended pixels; Quarter ~16×.
- Distortion: one extra scene-colour copy + one fullscreen pass (~0.1–0.3 ms at 1440p).
- Flipbook blending: 2 texture fetches instead of 1 per particle pixel.
- Emitter cap / spawn scale: linear in particle count and overdraw.

Effects quality matters most in **combat**: a GPU that holds 120 fps exploring can drop
to 60 during explosions when every effect fills the screen. That is why competitive
players often run effects on Low even with a strong GPU (also better visibility).

## Optimization

- Reduced-resolution effects + bilateral upsample is the single biggest win.
- Budget by **screen coverage**, not by count.
- Use simple shaders for distant effects (effect LOD).
- Pre-bake complex simulations (fluid fire) into flipbooks instead of simulating live.
- Pool and recycle emitters; avoid per-frame allocation.
- Merge distortion into the post-process chain so no extra copy is needed.

## Rebuild (from scratch)

```cpp
void applyVfxPreset(GeoVfxSettings& s) {
    static const struct { int pq, res; bool dist, blend; int emitters; float spawn; } k[4] = {
        {0, 2, false, false,  4, 0.25f},   // res: 0=Full 1=Half 2=Quarter
        {1, 1, false, true,   8, 0.50f},
        {2, 0, true,  true,  16, 1.00f},
        {3, 0, true,  true,  32, 1.00f}};
    auto& p = k[int(s.vfxQuality)];
    s.particleQuality = ParticleQuality(p.pq); s.vfxResolution = p.res;
    s.distortion = p.dist; s.flipbookBlend = p.blend;
    s.maxEmitters = p.emitters; s.spawnScale = p.spawn;
}
```
Render loop:
1. Downsample depth to the effects resolution (max or min depth per 2×2 block).
2. Bind the effects FBO (RGBA16F), clear to (0,0,0,1) for premultiplied alpha.
3. Draw particles with soft-particle test against the low-res depth.
4. Composite onto the HDR scene: `scene = scene * fx.a + fx.rgb`, with bilateral upsample.
5. If distortion is on: draw distortion sprites to an RG16F buffer and run the offset pass.
