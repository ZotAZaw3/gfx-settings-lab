# Decals

> Menu: **Effects → Decal Quality / Max Decals / Decal Lifetime**
> Setting ids: `effects.decalQuality` (Low / Medium / High), `effects.maxDecals` (16–2048), `effects.decalLifetime` (5–120 s) · Owner module: `geometry.decals`
> Presets: quality Low, Medium, High, High; max 64 / 128 / 256 / 1024; lifetime 10 / 20 / 30 / 60 s
> Files: `src/modules/geometry/decals.cpp`, `shaders/geometry/decal.{vert,frag}`, `decal_common.glsl`

## Definition

Decals are images projected onto existing surfaces: bullet holes, blood splatter,
scorch marks, footprints, tyre tracks, graffiti, puddles, road markings and grime.
They add variation and gameplay feedback without new geometry or unique textures.
The setting controls how many can exist, how long they stay, and how much material
data they modify (colour only, or colour + normal + roughness).

## How it works

Three generations:
1. **Mesh decals**: a small quad offset slightly above the surface. Cheap, but z-fights
   and does not wrap around corners.
2. **Projected mesh decals**: clip the scene's triangles against the decal box on the CPU
   and build a mesh that hugs the surface. Accurate but CPU heavy at spawn time.
3. **Deferred / screen-space box decals** (modern standard, used here): draw a unit cube
   oriented in the world. For each pixel the cube covers:
   - read scene depth → reconstruct the world position,
   - transform it into the decal's local space (`inverse(decalModel) * worldPos`),
   - if it lies outside the [-0.5, 0.5]³ box, `discard`,
   - use local `xz` as the decal UV, sample the decal texture,
   - blend the result into the **G-buffer** (albedo, normal, roughness) *before lighting*,
     so the decal is lit and shadowed exactly like the surface.
   Fading by `dot(surfaceNormal, decalForward)` stops decals stretching on walls
   perpendicular to the projection.

Clustered/forward engines use **clustered decals** instead: decals are binned into the
same light-cluster grid and each material shader loops over the decals in its cluster.

Lifetime management: a ring buffer of `maxDecals`; the oldest fade out and are replaced.

## Breakdown

**In this prototype:**

1. Stage `GBuffer`, order 50 — "Decals": copy `gbuffer.normal` and `gbuffer.material`, then one instanced draw of
   unit boxes into albedo (alpha blend, rgb only), normal (rgb only, shading-model id kept) and material (roughness
   only). `scene.depth` is **sampled, not attached**, so there is no feedback loop; each box keeps only its back faces
   (computed from the face normal, so it works with the camera inside the box).
2. Decals spawn on the plaza at a steady rate of `max / lifetime` per second and the oldest are recycled.


| Level  | Channels modified          | Normal-blend | Angle fade | Max decals |
|--------|----------------------------|--------------|------------|------------|
| Low    | albedo                     | no           | no         | 64         |
| Medium | albedo + normal            | yes          | yes        | 256        |
| High   | albedo + normal + roughness| yes          | yes        | 1024       |

Costs: each decal is a draw (or an instance) whose cost = pixels covered × (1 depth
fetch + texture fetches + G-buffer blend). Overlapping decals (a wall full of bullet
holes) multiply overdraw. CPU cost is tiny with instancing.

## Optimization

- Instance all decals of one atlas in a single draw (per-instance matrix + atlas rect).
- Render back faces with depth test `GEQUAL` reversed when the camera is inside the box;
  otherwise front faces with depth test on so hidden decals are rejected early.
- Use a texture atlas / array so all decals share one material.
- Merge static decals into the surface's texture (runtime virtual texturing) after they settle.
- Distance-cull decals aggressively; fade small ones first.
- Use a stencil bit to stop decals from drawing onto characters / dynamic objects.

## Rebuild (from scratch)

```glsl
// decal.frag (G-buffer pass, after opaque geometry, depth write off)
layout(binding=0) uniform sampler2D uDepth;
layout(binding=1) uniform sampler2D uDecalAlbedo;
uniform mat4 uInvViewProj; uniform mat4 uInvModel; uniform vec2 uInvViewport;
layout(location=0) out vec4 gAlbedo;
void main(){
    vec2 uv = gl_FragCoord.xy * uInvViewport;
    float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec4 wp = uInvViewProj * vec4(uv.x*2.0-1.0, 1.0-uv.y*2.0, d, 1.0); wp /= wp.w;
    vec3 lp = (uInvModel * wp).xyz;
    if (any(greaterThan(abs(lp), vec3(0.5)))) discard;
    vec4 c = texture(uDecalAlbedo, lp.xz + 0.5);
    float fade = 1.0 - smoothstep(0.35, 0.5, abs(lp.y));  // fade near box top/bottom
    gAlbedo = vec4(c.rgb, c.a * fade);
}
```
C++: bind the G-buffer FBO with only the albedo (and optionally normal/roughness)
attachments as draw targets, `glDepthMask(GL_FALSE)`, blending
`glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE)` so alpha
in the G-buffer is preserved, draw an instanced unit cube per decal. The depth texture
must be a copy (or the depth attachment detached) because you cannot sample the depth
buffer you are testing against.
