# MSAA (Multisample Anti-Aliasing)

> Menu: **Anti-Aliasing & Upscaling → MSAA** · Setting id(s): `aa.msaa.samples` (samples = 1 << value) · Owner module: `aa.temporal (setting), core (Forward pass)`
> Values: Off / 2x / 4x / 8x · Presets: Off at every preset

Setting shown in the menu: `Anti-Aliasing: MSAA 2x / 4x / 8x`

## Definition

MSAA removes jagged geometric edges by testing coverage at several sample points inside each pixel, while running the pixel shader only once per pixel per triangle. It fixes geometry edges only; it does nothing for shader aliasing (specular sparkle, alpha-tested foliage, thin normal-map detail).

## How it works

1. The colour and depth buffers store N samples per pixel (2, 4 or 8), at fixed sub-pixel positions (standard patterns, rotated grid).
2. Rasterisation tests the triangle against all N sample positions; depth/stencil is tested and written **per sample**.
3. The fragment shader runs **once** per pixel (at the pixel centre, or the centroid of covered samples) and its colour is written to every covered sample.
4. At the end, a **resolve** averages the N samples into one pixel (`glBlitFramebuffer` from the MSAA FBO to a normal one, or `ResolveSubresource` in D3D11).

So an edge pixel half covered by a red triangle and half by blue resolves to purple: a smooth edge. A pixel fully inside a triangle has all samples equal and costs the same as no AA.

## Breakdown

| | 2x | 4x | 8x |
|---|---|---|---|
| Edge gradient levels | 3 | 5 | 9 |
| Colour+depth memory (1080p, RGBA16F + D32) | 2x | 4x | 8x |
| Typical cost (forward renderer) | +10% | +20-30% | +50%+ |

Why modern engines rarely use it: **deferred rendering**. A G-buffer would need N samples per attachment (huge bandwidth) and lighting would have to run per sample on edges. Also, HDR resolve needs tone mapping per sample before averaging, or bright edges alias anyway.

In this prototype the setting `aa.msaa.samples` (Off/2x/4x/8x) switches the core renderer from deferred (G-buffer) to its forward path (`Stage::Forward`), which renders into a multisampled target and resolves it. That switch is itself the lesson: MSAA and deferred shading don't mix.

- OpenGL: `glTextureStorage2DMultisample(tex, samples, GL_RGBA16F, w, h, GL_TRUE)`, resolve with `glBlitNamedFramebuffer(..., GL_COLOR_BUFFER_BIT, GL_NEAREST)`.
- D3D11: `DXGI_SAMPLE_DESC{samples, 0}` on the render target, `ResolveSubresource`.
- Code: `src/modules/aa/temporal.cpp`, custom resolve `shaders/aa/msaa_resolve.frag`.

## Optimization

- 4x is the sweet spot. 8x costs a lot of bandwidth for small gain.
- **Tonemap-aware resolve:** in HDR, weight each sample by `1 / (1 + luma)` before averaging and un-weight after; avoids bright edges staying jagged. This prototype's custom resolve does that.
- **Alpha to coverage** (`GL_SAMPLE_ALPHA_TO_COVERAGE`) gives foliage and fences smooth alpha-tested edges for free when MSAA is on.
- Combine with FXAA/SMAA, or use TAA if you need shader-aliasing coverage.
- Tile-based mobile GPUs resolve MSAA on-chip almost for free; there it is still the best AA.

### Measured cost

Measured cost at 1920×1080 (`gfxlab --bench-settings`), GPU: **Mesa llvmpipe 25.2.8 (software rasterizer, CPU)**. Deltas are against the High preset (TAA, CAS, AF 16x, native). On llvmpipe a whole frame takes hundreds of ms and the run-to-run noise is about ±8 ms (changing a setting with no rendering effect moves it that much), so only large differences mean anything; on a real GPU every pass here costs well under 1 ms. Greyed-out vendor options (DLSS, FSR 3, XeSS) snap back to our version, so they aren't listed.

| Setting | Value | Δ GPU ms | Δ CPU ms | Δ VRAM MB | Δ RAM MB |
|---|---|---|---|---|---|
| MSAA | Off | -2.0 | -0.9 | +0.0 | +0.0 |
| MSAA | 2x | +98.3 | +99.7 | +63.3 | +127.3 |
| MSAA | 4x | +103.5 | +104.8 | +126.6 | +190.6 |
| MSAA | 8x | +100.6 | +100.7 | +126.6 | +190.6 |

## Rebuild (from scratch)

Supersampling (SSAA) is the brute-force ancestor: render at 2x width and height and downsample. MSAA is the optimisation of it. To rebuild MSAA without hardware support you would store N depth + coverage samples per pixel and shade once, which is exactly what the hardware does; so "rebuild" here means writing the resolve yourself:

```glsl
uniform sampler2DMS uColorMS; uniform int uSamples;
out vec4 o;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 sum = vec3(0); float wsum = 0.0;
    for (int i = 0; i < uSamples; i++) {
        vec3 c = texelFetch(uColorMS, p, i).rgb;
        float w = 1.0 / (1.0 + max(c.r, max(c.g, c.b)));   // tonemap weight
        sum += c * w; wsum += w;
    }
    o = vec4(sum / wsum, 1);
}
```

Steps:
1. Create a multisampled colour + depth FBO with the chosen sample count.
2. Draw the opaque scene into it.
3. Resolve to a single-sample HDR texture (hardware blit or the custom shader above).
4. Continue the post chain on the resolved texture.
5. On sample-count change, recreate the FBO (cannot change in place).
