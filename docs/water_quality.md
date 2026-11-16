# Water Quality

Menu: **World → Water Quality** (`Low | Medium | High | Ultra`),
**Water Tessellation** (on/off), **Caustics** (on/off)
Code: `GeoVfxSettings::waterQuality`, `waterTess`, `caustics`; shaders `geometry/water.*`

## Definition

Water quality controls how oceans, lakes and rivers are drawn: wave geometry and
animation, reflections and refraction, foam, depth colour, caustics (light patterns on
the bottom) and interaction (wakes, splashes).

## How it works

1. **Wave shape.**
   - Low: scrolling normal maps on a flat plane.
   - Medium: a sum of **Gerstner waves** in the vertex shader (each wave moves points in
     circles, giving sharp crests and flat troughs).
   - High/Ultra: **FFT ocean** (Tessendorf): a statistical wave spectrum (Phillips/JONSWAP)
     is evolved in frequency space and inverse-FFT'd every frame into displacement and
     normal textures, often several cascades at different scales.
2. **Geometry.** A projected grid or a tessellated/clipmapped mesh around the camera so
   waves have real height near the viewer.
3. **Shading.**
   - **Fresnel** (Schlick): reflection grows at grazing angles.
   - Reflection from SSR / planar reflection / probes (owned by the reflections thread).
   - **Refraction**: sample the scene colour behind the water with UV offset by the normal.
   - **Depth colour/absorption**: `exp(-absorption * waterDepth)` tints deep water.
   - **Foam** where the Jacobian of the displacement is < 0 (crests fold) and at shorelines
     (small depth difference).
   - **Subsurface** glow in thin crest regions facing the sun.
4. **Caustics**: animated texture or computed by refracting light rays through the
   surface; projected on the sea floor.

## Breakdown

| Level  | Waves                   | Geometry              | Refraction | Foam | Caustics |
|--------|-------------------------|-----------------------|------------|------|----------|
| Low    | normal maps             | flat plane            | none       | no   | no       |
| Medium | 4 Gerstner              | 64² grid              | yes        | crest| no       |
| High   | 8 Gerstner (or FFT 128²)| tessellated           | yes        | yes  | yes      |
| Ultra  | FFT 256² × 3 cascades   | tessellated, fine     | yes + blur | yes  | yes      |

Cost: FFT compute (small), vertex/tessellation, pixel (refraction requires a scene
colour copy; reflections are the big one).

## Optimization

- FFT at 128²–256² is enough; tile and use cascades for scale variety.
- Use a scene colour copy you already have (e.g. from distortion) for refraction.
- Fade wave detail and tessellation with distance.
- Reuse SSR; fall back to a reflection probe on Low.

## Rebuild (from scratch, Gerstner)

```glsl
vec3 gerstner(vec3 p, vec2 dir, float steep, float wavelength, float t, inout vec3 T, inout vec3 B){
    float k = 6.28318 / wavelength, c = sqrt(9.8 / k), f = k * (dot(dir, p.xz) - c * t);
    float a = steep / k;
    T += vec3(-dir.x*dir.x*steep*sin(f), dir.x*steep*cos(f), -dir.x*dir.y*steep*sin(f));
    B += vec3(-dir.x*dir.y*steep*sin(f), dir.y*steep*cos(f), -dir.y*dir.y*steep*sin(f));
    return vec3(dir.x * a * cos(f), a * sin(f), dir.y * a * cos(f));
}
// VS: vec3 T=vec3(1,0,0), B=vec3(0,0,1); p += gerstner(...) for each wave;
//     normal = normalize(cross(B, T));
// FS: F = 0.02 + 0.98*pow(1-dot(N,V),5); color = mix(refracted*absorption, reflected, F);
```
