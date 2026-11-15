# Texture Filtering (Nearest / Bilinear / Trilinear / Anisotropic)

> Menu: **Textures → Texture Filtering** · Setting id(s): `textures.filtering`, `textures.filtering.upscale_bias` · Owner module: `aa.texture_filtering`
> Values: Nearest / Bilinear / Trilinear / Anisotropic 2x-16x · Presets: Low=Trilinear, Medium=AF 4x, High=AF 16x, Ultra=AF 16x

Settings shown in the menu: `Texture Filtering: Nearest | Bilinear | Trilinear | Anisotropic 2x / 4x / 8x / 16x`

## Definition

Texture filtering is how the GPU decides what colour to return when a shader samples a texture at a position that does not land exactly on one texel, or when one screen pixel covers many texels (minification) or a fraction of one texel (magnification). Better filtering removes blockiness up close, shimmering and moiré at a distance, and blur on surfaces seen at grazing angles (floors, roads, walls running away from the camera).

## How it works

1. **Nearest (point):** take the single texel whose centre is closest. Free, but blocky when magnified and sparkles when minified.
2. **Bilinear:** take the 2x2 texels around the sample point and blend them by the fractional position. Smooth when magnified. When minified more than 2:1 it still only reads 4 texels, so it aliases.
3. **Mipmaps:** the texture is stored as a chain of pre-filtered half-size copies (1024, 512, 256 … 1). The GPU computes the screen-space derivatives of the UV (`ddx`, `ddy`), estimates how many texels one pixel covers (the footprint), and picks the mip level `lod = log2(footprint)`.
4. **Trilinear:** bilinear in the two nearest mip levels, then blend between them by the fractional LOD. Removes the visible "mip band" seams of bilinear + nearest-mip.
5. **Anisotropic (AF):** at grazing angles the pixel footprint in texture space is a long thin ellipse, not a square. Trilinear has to pick the mip for the *long* axis, which blurs the short axis. AF instead picks the mip for the *short* axis and takes N trilinear probes along the long axis (N up to the anisotropy level: 2, 4, 8, 16) and averages them. Result: sharp floors far into the distance.

## Breakdown

| Mode | Texel reads / sample (worst case) | Quality |
|---|---|---|
| Nearest | 1 | Blocky, sparkly |
| Bilinear (+nearest mip) | 4 | Smooth, mip bands visible |
| Trilinear | 8 | No bands, blurry at grazing angles |
| AF 4x | up to 32 | Sharp to mid distance |
| AF 16x | up to 128 | Sharp to horizon |

Real hardware only takes as many probes as the actual anisotropy of that pixel requires, so 16x is usually only a little slower than 4x.

In this prototype:

- OpenGL: `glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR)` and `glTextureParameterf(tex, GL_TEXTURE_MAX_ANISOTROPY, n)` (core in GL 4.6, `EXT_texture_filter_anisotropic` before). Better: one sampler object per mode, `glBindSampler`.
- Direct3D 11: `D3D11_SAMPLER_DESC.Filter = D3D11_FILTER_ANISOTROPIC; MaxAnisotropy = n;` or `D3D11_FILTER_MIN_MAG_MIP_LINEAR` for trilinear.
- Code: `src/modules/aa/texture_filtering.cpp` builds a sampler set and re-applies it when the setting changes.

## Optimization

- AF is almost free on modern GPUs; 16x is the right default on desktop. Drop to 4x on low-end or mobile.
- Always generate mipmaps (`glGenerateTextureMipmap`, or pre-baked in DDS/KTX2). Without mips, minification thrashes the texture cache and costs more than AF.
- Do **not** use AF for post-processing and full-screen buffers; use plain bilinear, no mips (they are sampled 1:1).
- When using upscaling (TAA-U/FSR/DLSS), apply a **negative mip bias** of `log2(renderWidth / outputWidth)` (e.g. -1 at 50% scale) so textures are sampled at output-resolution detail. The upscaler accumulates the extra detail. This prototype does it automatically when render scale < 1.
- Block-compressed formats (BC1/BC3/BC7, ASTC) halve or quarter bandwidth and filter in hardware at full speed.

## Rebuild (from scratch)

Software trilinear + anisotropic sampler in a shader (useful to understand what the hardware does; `shaders/aa/manual_aniso.glsl`):

```glsl
vec4 bilinear(sampler2D t, vec2 uv, float lod) {      // one hardware tap with forced LOD
    return textureLod(t, uv, lod);                    // hardware still does the 2x2 blend
}
vec4 trilinear(sampler2D t, vec2 uv, float lod) {
    float l0 = floor(lod);
    return mix(textureLod(t, uv, l0), textureLod(t, uv, l0 + 1.0), lod - l0);
}
vec4 anisotropic(sampler2D t, vec2 uv, float maxAniso) {
    vec2 size = vec2(textureSize(t, 0));
    vec2 dx = dFdx(uv) * size, dy = dFdy(uv) * size;      // footprint axes in texels
    float lx = length(dx), ly = length(dy);
    float pMax = max(lx, ly), pMin = min(lx, ly);
    float n = min(ceil(pMax / max(pMin, 1e-6)), maxAniso); // probes needed
    float lod = log2(pMax / n);                           // mip chosen by the short axis
    vec2 axis = (lx > ly ? dx : dy) / size;               // long axis back in UV space
    vec4 sum = vec4(0);
    for (float i = 0.0; i < n; i++) {
        float o = (i + 0.5) / n - 0.5;                    // spread probes along the axis
        sum += trilinear(t, uv + axis * o, max(lod, 0.0));
    }
    return sum / n;
}
```

Steps to rebuild the feature in an engine:
1. Generate mip chains for every material texture at load time.
2. Create sampler objects: nearest, bilinear, trilinear, AF2/4/8/16 (query `GL_MAX_TEXTURE_MAX_ANISOTROPY` for the cap).
3. Bind the selected sampler to every material texture unit; full-screen passes keep their own linear-clamp sampler.
4. Add mip LOD bias = `log2(renderScale)` when temporal upscaling is on.
5. Test scene: an infinite checkerboard floor at grazing angle. Toggle modes and watch the horizon.
