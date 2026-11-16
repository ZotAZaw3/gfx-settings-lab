# Level of Detail (LOD) and Draw Distance

> Menu: **Geometry → LOD Bias / LOD Hysteresis / Draw Distance**, **World → Geometry Showcase Area / Jump Camera To**
> Setting ids: `geometry.lodBias` (−2…3), `geometry.lodHysteresis`, `geometry.drawDistance` (50–2000 m), `world.showcase`, `world.showcaseCamera` · Owner module: `geometry.lod`
> Presets: LOD Bias Low=+1, Medium=0, High=0, Ultra=−1 · Draw Distance 150 / 300 / 600 / 1200 m
> Files: `src/modules/geometry/lod_culling.cpp`

## Definition

**LOD** swaps each object's mesh for a simpler version as it gets smaller on screen,
because detail you cannot see is wasted work. **Draw distance** is the far limit beyond
which objects are not drawn at all (or are replaced by impostors / fog). Most games expose
both, plus separate distances for small "detail" objects like grass, rocks and debris.

## How it works

1. **Each object has an LOD chain** (`LOD0` full … `LOD3` ~12 % triangles), see
   `mesh_quality.md`.
2. **Screen-size metric.** Every frame, for each visible object:
   ```
   projectedRadius_px = (boundingRadius / distance) * (screenHeight / (2*tan(fovY/2)))
   ```
   Thresholds (e.g. 200 px → LOD0, 80 px → LOD1, 30 px → LOD2, else LOD3) choose the
   level. Using *projected size* instead of raw distance makes LOD correct across FOV
   and resolution changes (zooming a sniper scope keeps LOD0).
3. **LOD bias** shifts the result: `lod = clamp(computed + bias + baseLodFromMeshQuality)`.
   Negative = sharper, positive = faster.
4. **Transitions.** A hard switch "pops". Two common fixes:
   - **Dithered cross-fade**: for ~0.3 s both LODs are drawn; one discards pixels with
     an ordered-dither pattern at `alpha`, the other at `1-alpha`. No sorting or blending needed.
   - **Geomorphing**: vertices of the fine mesh slide to their parent positions (used on terrain).
   - **Hysteresis**: switch down at 30 px but back up only at 36 px so objects near the
     threshold do not flicker between levels.
5. **Draw distance** = far cull. Objects whose nearest bounding point is farther than
   `drawDistance` are skipped. Detail objects use `drawDistance * detailScale * objectSize`
   so small things vanish first. Fog (see `volumetric_fog_clouds.md`) hides the cutoff.
6. **Far field**: past the last LOD, games use **impostors** (a camera-facing quad with a
   pre-rendered octahedral atlas of the object) or **HLOD** (merged proxy meshes for whole
   city blocks).

## Breakdown

**In this prototype:**

1. Stage `FrameSetup` — `geometry.lod::filterDraws` (runs after core fills the DrawList at order −100):
   draw-distance cull → frustum cull → small-object cull → CPU occlusion → LOD pick → writes back `opaque`/`transparent`.
2. LOD pick in this prototype uses the **projected simplification error** rather than fixed pixel-radius thresholds:
   `errPx = lod.error * scale / distance * proj[1][1] * height/2`; the coarsest level with `errPx ≤ 1 px · 2^bias` wins
   (the same rule Nanite uses per cluster). Hysteresis requires 20 % extra margin before switching coarser than last frame.
3. The showcase "LOD field" (≈350 dense torus knots, 15k triangles each at LOD0) is added to the scene so you can watch
   the triangle counter (`lod.triangles` in the overlay) react to LOD Bias and Model Quality.


| Knob            | GPU effect                                   | CPU effect                      |
|-----------------|----------------------------------------------|---------------------------------|
| LOD bias −      | more triangles, tiny-triangle waste          | none                            |
| Dither fade     | 2 draws per transitioning object for 0.3 s   | tracking fade state             |
| Draw distance + | more objects drawn, more shadow casters      | more culling / draw calls (big) |
| Detail distance | many small instanced objects, overdraw (grass) | instance buffer building      |

Draw distance is usually the biggest CPU cost setting in open-world games, because
draw-call submission scales with visible object count.

## Optimization

- Select LOD on the GPU in the culling compute shader (see `culling.md`) and write an
  indirect draw per LOD bucket: zero CPU cost per object.
- Use one LOD lower for shadow maps and reflections.
- Always combine draw distance with fog/atmospheric scattering so the cutoff is invisible.
- Instance identical meshes; build per-LOD instance lists.
- Hysteresis + dithering stops visible popping without costly alpha blending.
- Stream far-away LODs only; load LOD0 when the object comes within range.

## Rebuild (from scratch)

```cpp
int selectLod(const Object& o, const Camera& c, const GeoVfxSettings& s) {
    float d = length(o.center - c.pos);
    if (d - o.radius > s.drawDistance * (o.isDetail ? s.detailDistanceScale : 1.f))
        return -1;                                        // culled by distance
    float px = (o.radius / max(d, 0.01f)) * (c.viewportH / (2.f * tanf(c.fovY * 0.5f)));
    static const float kThresh[3] = {200.f, 80.f, 30.f};
    int lod = 3;
    for (int i = 0; i < 3; ++i) if (px > kThresh[i]) { lod = i; break; }
    lod += s.lodBias + baseLod(s.meshQuality);
    return clamp(lod, 0, 3);
}
```
Dither cross-fade fragment snippet:
```glsl
uniform float uFade;    // 0..1, sign tells which side of the fade this draw is
float bayer4(ivec2 p){ const float m[16]=float[](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);
                       return (m[(p.x&3)+(p.y&3)*4]+0.5)/16.0; }
void lodDither(){ float t=bayer4(ivec2(gl_FragCoord.xy));
                  if (uFade>0.0 ? t>uFade : t<1.0+uFade) discard; }
```
Draw the outgoing LOD with `uFade = +(1-a)` and incoming with `uFade = -(1-a)`, where
`a` rises 0→1 over the transition.
