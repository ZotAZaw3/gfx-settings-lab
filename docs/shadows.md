# Shadows

Covers: shadow maps, cascaded shadow maps (CSM), PCF, PCSS (soft contact-hardening), and screen-space contact shadows.
Code: `src/modules/lighting/ShadowModule.cpp`, `shaders/lighting/shadow_depth.{vert,frag}`, `shaders/lighting/shadow_sample.glsl`, `shaders/lighting/contact_shadows.comp`.

## Definition

A shadow is the region a light cannot reach because something sits between it and the surface. In a rasterizer nothing knows about occlusion by default: each pixel is shaded from its own normal and the light direction only. Shadow techniques add a visibility term `V ∈ [0,1]` that multiplies the light's contribution.

| Setting value | What you get |
|---|---|
| Off | `V = 1` everywhere. Objects float. |
| Low | 1 cascade-less 1024² map, 1-tap hardware PCF |
| Medium | 3 cascades × 1024², 3×3 PCF |
| High | 4 cascades × 2048², 5×5 Poisson PCF + contact shadows |
| Ultra | 4 cascades × 4096², PCSS (variable penumbra) + contact shadows |

### Settings in this app

| Menu label | Setting id | Values |
|---|---|---|
| Shadow Quality | `lighting.shadows.quality` | Off / Low / Medium / High / Ultra |
| Shadow Cascades | `lighting.shadows.cascades` | 1-4 |
| Shadow Filtering | `lighting.shadows.filter` | Hardware PCF / PCF 3x3 / Poisson PCF / PCSS |
| Sun Softness (PCSS) | `lighting.shadows.lightSize` | 0.05-2 |
| Contact Shadows | `lighting.shadows.contact` | on/off |

## How it works

### 1. Shadow map
1. Render the scene from the light's point of view, writing only depth into a depth texture (the *shadow map*).
2. When shading a pixel in the main pass, transform its world position into light clip space: `p_light = LightViewProj * worldPos`.
3. Compare the pixel's depth from the light with the depth stored in the map at that texel. If the stored depth is closer, something blocks the light: `V = 0`.

### 2. Cascaded shadow maps
A single map covering a whole outdoor view gives huge blurry texels near the camera. CSM splits the camera frustum into N slices along view depth and fits one orthographic shadow map per slice. Near slices are small, so their texels are dense.

Split distances use the "practical" blend of logarithmic and uniform splits:

```
split_i = λ · n·(f/n)^(i/N) + (1-λ) · (n + (f-n)·i/N)     λ ≈ 0.75
```

Each cascade's ortho matrix is fit to the bounding sphere of its frustum slice (sphere = rotation-stable, no shimmer) and snapped to texel increments so the map does not crawl when the camera moves.

### 3. PCF (Percentage-Closer Filtering)
Instead of one binary test, take several depth comparisons around the lookup point and average them. That gives a soft edge of fixed width. `sampler2DShadow` with `GL_COMPARE_REF_TO_TEXTURE` and linear filtering gives a free 2×2 bilinear PCF per tap.

### 4. PCSS (Percentage-Closer Soft Shadows)
Real penumbrae widen with the distance between occluder and receiver (a pole's shadow is sharp at the base, soft at the tip).
1. **Blocker search:** sample the map in a region around the pixel, average the depths that are closer than the receiver → `d_blocker`.
2. **Penumbra estimate:** `w = (d_receiver − d_blocker) / d_blocker · lightSize`.
3. **Filter:** run PCF with kernel radius `w`.

### 5. Contact shadows
Shadow maps miss small detail (fingers, grass blades, wrinkles) because of limited resolution and bias. Contact shadows march a short ray in screen space from each pixel toward the light, using the depth buffer. If the ray goes behind the depth buffer within a small thickness, the pixel is occluded. Cheap and sharp, but only works for things visible on screen.

## Breakdown

| Stage | Pass | Inputs | Output | Cost driver |
|---|---|---|---|---|
| Cascade fit | CPU | camera frustum, light dir | N light matrices | negligible |
| Depth render | N depth-only passes | scene geometry | `GL_TEXTURE_2D_ARRAY` depth, N layers | draw calls × cascades, resolution² |
| Cascade select | lighting shader | view depth | cascade index | 1 compare per cascade |
| Filter | lighting shader | shadow array | `V` | taps per pixel (1 / 9 / 25 / 16+32) |
| Contact shadows | fullscreen | depth, light dir | R8 mask | steps per pixel |

Artifacts and their fixes:
- **Shadow acne** (striped self-shadowing): depth precision vs. slope. Fix with slope-scaled bias (`glPolygonOffset`) plus normal offset (push the lookup point along the normal by ~1 texel).
- **Peter-panning** (shadow detached from the object): too much bias. Use normal offset instead of large constant bias.
- **Shimmering** on camera move: snap cascade origin to texel grid; use sphere bounds.
- **Cascade seams**: blend the last 10% of each cascade with the next one.

## Optimization

- **Front-face culling or no culling** in the depth pass removes most acne on closed meshes.
- **Depth-only pipeline**: no color attachment, trivial fragment shader (or none), position-only vertex stream.
- **Layered rendering**: render all cascades in one pass with a geometry shader writing `gl_Layer`, or instanced rendering with `gl_Layer` from the vertex shader (`GL_ARB_shader_viewport_layer_array`). Saves N−1 sets of state changes.
- **Cull per cascade**: test each object against the cascade's ortho box, not the whole camera frustum.
- **Update far cascades less often**: cascades 2–3 every other frame; they change little.
- **Static/dynamic split**: cache static geometry in one map, re-render only dynamic objects on top.
- **Poisson disk + per-pixel rotation** gives smoother PCF than a grid at the same tap count; TAA (from the AA thread) cleans up the noise.
- **Early-out**: if the four corner taps agree, skip the inner taps (all lit or all shadow).
- **Resolution budget**: the biggest knob. 4096² × 4 cascades = 256 MB of 32F depth; use `GL_DEPTH_COMPONENT16` for far cascades.

### Measured cost

Pending: the per-value table (Δ GPU ms, Δ CPU ms, Δ VRAM, Δ RAM at 1920×1080) is generated by `gfxlab --bench-settings docs/_measurements` once the engine builds, and will be pasted here.

## Rebuild (from scratch)

1. Create a `GL_TEXTURE_2D_ARRAY` with `GL_DEPTH_COMPONENT32F`, `N` layers, set `GL_TEXTURE_COMPARE_MODE = GL_COMPARE_REF_TO_TEXTURE`, `GL_TEXTURE_COMPARE_FUNC = GL_LEQUAL`, linear filter, clamp-to-border with border = 1.0.
2. Create an FBO; for each cascade attach layer `i` with `glFramebufferTextureLayer`, `glDrawBuffer(GL_NONE)`.
3. CPU: compute split distances with the formula above. For each slice, take the 8 frustum corners in world space, compute bounding sphere (center, radius). Build `lookAt(center − lightDir·radius·2, center, up)` and `ortho(−r, r, −r, r, 0, 4r)`. Snap: project world origin to shadow texel space, round, offset the matrix by the fractional part.
4. Render: for each cascade, bind layer, clear depth, `glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(2, 4)`, draw scene with a depth-only shader using that cascade's matrix.
5. In the lighting shader: pick cascade `i` = first split with `viewDepth < split[i]`. Compute `p = LightVP[i] * (worldPos + N * normalOffset * texelSize[i])`, `uvz = p.xyz * 0.5 + 0.5`.
6. Sample: `texture(sampler2DArrayShadow, vec4(uvz.xy, i, uvz.z))` for hardware PCF; loop over a Poisson disk for wider kernels; add blocker search for PCSS.
7. Contact shadows: fullscreen pass, reconstruct view position from depth, march 16 steps of `maxDist/16` toward the light in view space, project each step to screen, compare with depth buffer, mark occluded if `sceneDepth < rayDepth && rayDepth − sceneDepth < thickness`. Multiply the result into `V`.
8. Expose `shadowQuality` in settings → choose cascade count, resolution, filter mode, contact shadow on/off.

> Engine note: the main camera uses reversed-Z, but shadow maps here use standard Z (clear 1.0, `GL_LESS`/`LEQUAL` compare). Orthographic cascades gain nothing from reversed-Z because their depth is already linear, and a separate convention keeps the compare sampler simple. The module sets and restores the depth function around its pass.
