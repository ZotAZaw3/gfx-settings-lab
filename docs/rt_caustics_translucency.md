# RT Translucency, Refraction and Caustics

Settings (Ray Tracing tab): refraction is part of `Path Tracing` (alpha-blended materials
become glass with transmission = 1 - alpha, IOR 1.5). `RT Caustics (photon)` is greyed out
(documented only).
Prerequisite reading: [raytracing_overview.md](raytracing_overview.md),
[path_tracing.md](path_tracing.md).

## Definition

- **Translucency / refraction**: light passing *through* a surface (glass,
  water, ice, gems) bends according to the index of refraction (IOR). Raster
  games fake it with a screen-space distortion of the scene behind, which
  can't show anything off screen and has no real thickness. RT translucency
  traces the refracted ray into the scene.
- **Caustics**: the bright, focused patterns light makes after refracting or
  reflecting off curved surfaces: the dancing lines at the bottom of a pool,
  the bright spot under a glass of water. They're notoriously hard: they need
  a path from the light *through* a specular surface onto a diffuse one.

## How it works

### Refraction

At a transmissive hit with normal `N` and IOR ratio `η = n1/n2`:

```glsl
float cosI = dot(-I, N);
float k = 1.0 - eta*eta*(1.0 - cosI*cosI);
vec3 T = (k < 0.0) ? reflect(I, N)                    // total internal reflection
                   : eta*I + (eta*cosI - sqrt(k))*N;
float F = fresnelSchlick(cosI, F0);                    // reflect with prob F
```

In a path tracer: with probability `F` follow the reflection, else the
refraction. Flip `N` and invert `η` when exiting. Apply **Beer-Lambert
absorption** for coloured glass: `throughput *= exp(-sigmaA * distance)`.

Hybrid (non-path-traced) RT translucency: from the glass G-buffer pixel trace
1-2 refraction rays, shade the hit like an RT reflection hit, and composite
weighted by `(1 - F)`.

### Caustics

Three approaches:

1. **Path tracing (implicit)**: a pure path tracer gets caustics for free when
   a camera path refracts and then happens to hit the light. Converges very
   slowly (the sun is small); with NEE, caustics *through glass* are missed
   because the shadow ray is blocked by the glass. That's the classic path
   tracing caustics problem. This prototype renders caustics only from
   BSDF-sampled paths that escape to the sky/hit emissive surfaces.
2. **Photon mapping / light tracing**: shoot rays *from the light*, follow
   them through specular surfaces, deposit "photons" where they land on
   diffuse surfaces (into a screen-space or world-space buffer), then blur.
   This is how real-time RT caustics are done (Unreal's RTX caustics branch,
   NVIDIA's "Caustics" sample).
3. **Water-specific**: project the water surface's refracted sun rays onto the
   floor via a texture (classic fake, no RT needed).

## Breakdown

| Feature | Cost |
|---|---|
| Refraction, hybrid, 1 ray | similar to RT reflections, 1-3 ms |
| Each extra transmission bounce | +1 closest-hit trace |
| Photon caustics, 256k photons | 1-3 ms trace + splat + blur |

## Optimization

- **Limit transmission bounces** (2 is enough for a glass pane: enter + exit).
- **Thin-surface approximation** for windows: refract in and immediately out
  with no offset, one ray.
- **Screen-space refraction first**, RT only for off-screen fallbacks.
- **Photon caustics only near caustic casters** (mark water/glass instances,
  shoot photons only through their bounding boxes).
- **Temporal accumulation** of the caustics buffer.

## Rebuild (from scratch)

1. Add `transmission` and `ior` to materials.
2. In the path tracer's BSDF sampling: if transmissive, choose reflect vs.
   refract by Fresnel, flip normal on exit, apply Beer-Lambert.
3. For caustics, add a light-tracing pre-pass: from the sun, sample points on
   the caustic casters' AABB, trace, refract, trace again, splat the hit's
   energy into a world/screen-space caustic buffer; blur; add to diffuse.

Status in this app: refraction through `transmission` materials is in the path
tracer; the photon caustics pass is documented only.
