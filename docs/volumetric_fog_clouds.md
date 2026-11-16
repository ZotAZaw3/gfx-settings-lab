# Volumetric Fog and Volumetric Clouds

Menu: **Effects → Volumetric Fog** (`Off | Low | Medium | High | Ultra`),
**Fog Density** (0 – 0.1), **Volumetric Clouds** (`Off | Low | Medium | High`),
**Cloud Coverage** (0 – 1)
Code: `GeoVfxSettings::volFog`, `fogDensity`, `clouds`, `cloudCoverage`;
shader `volumetrics.frag` (fullscreen ray-march)

## Definition

Volumetric fog simulates light scattering through air that contains dust, mist or smoke.
Unlike old "distance fog" (fade to a colour by depth), volumetric fog is lit: you see
shafts of light through windows and trees (god rays), shadows cast *into* the fog, and
glow around lamps. Volumetric clouds apply the same idea at sky scale: real 3D clouds
that the sun lights from inside and that cast shadows and change with time of day.

## How it works

Both are solutions of the **participating media** equation, by **ray marching**:

```
for each step along the view ray (from camera to scene depth or to max distance):
    density  = fogDensity(pos)                     // height falloff + 3D noise
    shadow   = sampleShadowMap(pos)                // is the sun visible from here?
    inScatter = sunColor * shadow * phase(cosθ) + ambient
    transmittance *= exp(-density * stepLen)       // Beer–Lambert
    color += transmittance * density * inScatter * stepLen
final = scene * transmittance + color
```
- **Phase function** (Henyey–Greenstein, `g≈0.6`) makes fog brighter when looking toward
  the sun (forward scattering), which is where god rays come from.
- **Froxel approach** (AAA standard: Frostbite, UE4/5): instead of per-pixel marching,
  divide the view frustum into a 3D grid (e.g. 160 × 90 × 64 "froxels"), compute
  scattering per froxel in a compute shader, integrate front-to-back along Z, and then
  every pixel (and every transparent object) just does one 3D texture lookup.
- **Temporal reprojection**: jitter the start offset per frame and blend with the
  previous frame's result. 8 steps with jitter + TAA ≈ 64 steps without.

**Clouds** march a slab between, say, 1500 m and 4000 m altitude: density =
`coverage`-remapped Perlin–Worley noise × height gradient, eroded by high-frequency
Worley detail. Light is integrated toward the sun with a few steps for self-shadowing
("powder" term darkens edges facing the viewer). Clouds are usually rendered at 1/4
resolution and updated over 16 frames (one pixel of each 4×4 block per frame), then
temporally reconstructed.

## Breakdown

| Fog level | Steps / froxel Z | Resolution     | Shadows in fog | Temporal |
|-----------|------------------|----------------|----------------|----------|
| Off       | analytic height fog only (no shadows) | –  | –         | –        |
| Low       | 16               | quarter        | no             | yes      |
| Medium    | 32               | half           | yes            | yes      |
| High      | 48               | half           | yes            | yes      |
| Ultra     | 96               | full           | yes            | yes      |

| Clouds    | Primary steps | Light steps | Res     |
|-----------|---------------|-------------|---------|
| Off       | 2D skybox     |             |         |
| Low       | 32            | 3           | quarter |
| Medium    | 64            | 4           | quarter |
| High      | 128           | 6           | half    |

Cost scales with **steps × pixels × (noise lookups + shadow lookups)**. Volumetric fog
on Ultra can cost 2–4 ms at 4K; clouds 1–3 ms. Both are among the best "drop one level
for big fps" settings in modern games.

## Optimization

- Froxels instead of per-pixel marching; resolution independent of screen size.
- Blue-noise or interleaved jitter + temporal accumulation.
- Render at reduced resolution, upsample with depth-aware filter.
- Early-out when transmittance < 0.01; skip empty space using a coarse density grid.
- Bake the 3D cloud noise to small tiled 3D textures (128³ base + 32³ detail).
- Amortize clouds over several frames (checkerboard / 4×4 reprojection).
- Share the shadow lookup with the god-ray effect (owned by the post-processing thread).

## Rebuild (from scratch)

Fullscreen pass after the opaque scene, reading the depth buffer:
```glsl
uniform sampler2D uDepth, uScene; uniform sampler2DShadow uShadow;
uniform mat4 uInvViewProj, uLightViewProj; uniform vec3 uCam, uSunDir, uSunCol;
uniform float uDensity; uniform int uSteps; uniform float uFrame;
float hg(float c, float g){ float g2=g*g; return (1.0-g2)/(4.0*3.14159*pow(1.0+g2-2.0*g*c,1.5)); }
void main(){
    float d = texture(uDepth, vUV).r;
    // gfxlab conventions: uv (0,0) top-left, ZERO_TO_ONE depth, reversed-Z (sky = 0)
    vec4 wp = uInvViewProj * vec4(vUV.x*2.0-1.0, 1.0-vUV.y*2.0, max(d,1e-6), 1.0); wp /= wp.w;
    vec3 rd = wp.xyz - uCam; float len = min(length(rd), 300.0); rd = normalize(rd);
    float stepLen = len / float(uSteps);
    float jitter = fract(sin(dot(gl_FragCoord.xy + uFrame, vec2(12.9898,78.233)))*43758.5453);
    vec3 col = vec3(0); float T = 1.0;
    float ph = hg(dot(rd, -uSunDir), 0.6);
    for (int i = 0; i < uSteps; ++i){
        vec3 p = uCam + rd * (float(i)+jitter) * stepLen;
        float dens = uDensity * exp(-max(p.y,0.0)*0.08);
        vec4 ls = uLightViewProj * vec4(p,1); ls.xyz = ls.xyz/ls.w*0.5+0.5;
        float vis = texture(uShadow, ls.xyz);
        vec3 S = (uSunCol * vis * ph + vec3(0.03,0.04,0.06)) * dens;
        float Tstep = exp(-dens*stepLen);
        col += T * (S - S*Tstep) / max(dens,1e-5);   // energy-conserving integration
        T *= Tstep; if (T < 0.01) break;
    }
    fragColor = vec4(texture(uScene, vUV).rgb * T + col, 1.0);
}
```
If the core engine has no shadow map yet, set `vis = 1.0` (fog without light shafts).
Clouds use the same loop restricted to the cloud slab, with noise-based density and a
short secondary march toward the sun for `vis`.
