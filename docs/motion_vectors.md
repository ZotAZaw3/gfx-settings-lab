# Motion Vectors (Velocity Buffer)

> Menu: **Debug → AA Debug View → Motion vectors** · Setting id(s): `aa.debug.view` · Owner module: `aa.post (view), core (generation)`
> Values: Off / Motion vectors / History only / History rejection / Upscaler confidence / RR-lite history length / Frame gen source · Presets: not in presets

Setting shown in the menu: `Debug View: Motion Vectors` (always generated when TAA, upscaling, motion blur, ray reconstruction or frame generation is on).

## Definition

A motion vector says, for every pixel on screen, where the surface under that pixel was on screen in the previous frame. It is the foundation of every temporal technique: TAA, temporal upscalers (DLSS, FSR 2/3, XeSS), ray reconstruction / temporal denoisers, frame generation, motion blur and temporal reprojection of SSR/SSAO.

## How it works

For each vertex, the vertex shader transforms the position twice:

- `currClip = currViewProj * currModel * pos` (as normal)
- `prevClip = prevViewProj * prevModel * prevPos` (last frame's matrices; `prevPos` differs from `pos` only for skinned or vertex-animated meshes)

Both are passed to the fragment shader, divided by `w` to get NDC, and the difference is written to an `RG16F` target:

```
velocity = currNDC.xy - prevNDC.xy             // NDC units (engine convention)
```

Important details:
- **Jitter must be removed.** When TAA or an upscaler jitters the projection, compute velocity with *unjittered* matrices (or subtract `currJitter - prevJitter`). Otherwise static scenes show fake motion and the history smears.
- **Camera-only motion** (static geometry) can be reconstructed from depth alone: `prevClip = prevViewProj * inverse(currViewProj) * (uv, depth)`. Used for the sky and for passes that have no geometry.
- **Convention used in this engine:** `scene.velocity` (RG16F) stores `currentNDC - previousNDC`. Clip space is D3D-style (`glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)`), so NDC y = +1 is the top of the screen and uv (0,0) is top-left. Converting to UV: `uvDelta = velocity * vec2(0.5, -0.5)` and `prevUV = uv - uvDelta`. All temporal shaders do this through `velocityToUV()` in `shaders/aa/common_temporal.glsl`. FSR expects `previous - current` in UV and DLSS takes a scale factor, so the sign/scale is adapted at the SDK boundary.
- **Depth is reversed-Z** (near = 1, far = 0). "Closest" in dilation means the *largest* depth value.

## Breakdown

| Piece | Where | Cost |
|---|---|---|
| prev matrices per object | CPU, stored after each frame | negligible |
| extra varying + RG16F MRT | G-buffer / forward pass | ~4 bytes/pixel write |
| depth-only reconstruction (sky, particles) | full-screen pass | ~0.05 ms at 1080p |
| dilation (nearest-depth 3x3) | consumed by TAA/upscaler | ~0.1 ms |

Code: core writes `scene.velocity` in the G-buffer / forward pass (`shaders/core/`). This module adds the debug view (`src/modules/aa/post_aa.cpp`, `shaders/aa/debug_velocity.frag`, setting Debug → AA Debug View → Motion vectors) and the shared helpers in `shaders/aa/common_temporal.glsl`. `shaders/aa/velocity_camera.frag` is a reference implementation of camera-only velocity from depth.

## Optimization

- Use `RG16F` (half precision is enough for sub-pixel accuracy up to 4K).
- Only write object velocity for moving objects; fill the rest with the depth-based camera-motion pass (lets static geometry skip the extra varying).
- **Dilate** before use: sample the 3x3 neighbourhood, take the velocity of the closest-depth pixel. This keeps the foreground's motion on its anti-aliased edges and removes ghost trails behind thin objects.
- Transparent / particle surfaces don't write velocity; mark them with a reactive mask (FSR) or let the TAA neighbourhood clamp handle them.

### Measured cost

Measured cost at 1920×1080 (`gfxlab --bench-settings`), GPU: **Mesa llvmpipe 25.2.8 (software rasterizer, CPU)**. Deltas are against the High preset (TAA, CAS, AF 16x, native). On llvmpipe a whole frame takes hundreds of ms and the run-to-run noise is about ±8 ms (changing a setting with no rendering effect moves it that much), so only large differences mean anything; on a real GPU every pass here costs well under 1 ms. Greyed-out vendor options (DLSS, FSR 3, XeSS) snap back to our version, so they aren't listed.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| AA Debug View | Off | +5.1 | +4.9 | +0.0 | +0.0 |
| AA Debug View | Motion vectors | +3.9 | +4.0 | +0.0 | +0.0 |
| AA Debug View | History only | -1.0 | -2.4 | +0.0 | +0.0 |
| AA Debug View | History rejection | -8.8 | -9.3 | +0.0 | +0.0 |
| AA Debug View | Upscaler confidence | +0.0 | -0.7 | +0.0 | +0.0 |
| AA Debug View | RR-lite history length | +0.6 | -0.1 | +0.0 | +0.0 |
| AA Debug View | Frame gen source | -2.5 | -2.4 | +0.0 | +0.0 |

## Rebuild (from scratch)

```glsl
// vertex
uniform mat4 uViewProj, uPrevViewProj, uModel, uPrevModel;   // unjittered VPs for velocity
uniform mat4 uViewProjJittered;
out vec4 vCurr, vPrev;
void main() {
    vec4 w  = uModel * vec4(aPos, 1);
    vCurr   = uViewProj * w;
    vPrev   = uPrevViewProj * (uPrevModel * vec4(aPos, 1));
    gl_Position = uViewProjJittered * w;
}
// fragment
layout(location = 2) out vec2 oVelocity;
void main() {
    vec2 c = vCurr.xy / vCurr.w, p = vPrev.xy / vPrev.w;
    oVelocity = c - p;                       // NDC delta, current minus previous
}
```

Steps:
1. Every object stores last frame's model matrix; the camera stores last frame's unjittered view-projection.
2. Add an RG16F attachment to the scene FBO; clear to 0.
3. Write velocity in every opaque geometry pass.
4. Run the camera-motion pass where depth == far plane (sky; depth 0 with reversed-Z).
5. Debug view: map `velocity * 50` to colour (`hsv(angle, 1, length)`) to check direction and scale.
6. At the end of the frame copy current matrices into the "prev" slots (and on camera cut, set prev = current to avoid a frame of huge vectors).
