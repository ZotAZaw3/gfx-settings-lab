// Software trilinear + anisotropic filtering, for the "Texture Filtering: Manual (educational)" debug mode.
// Shows what the hardware sampler does. Bind a sampler with LINEAR_MIPMAP_NEAREST? No: LINEAR min/mag,
// mips present; textureLod gives the hardware 2x2 bilinear for a forced level.
vec4 aaTrilinear(sampler2D t, vec2 uv, float lod) {
    float l0 = floor(lod);
    return mix(textureLod(t, uv, l0), textureLod(t, uv, l0 + 1.0), lod - l0);
}
vec4 aaAnisotropic(sampler2D t, vec2 uv, float maxAniso) {
    vec2 size = vec2(textureSize(t, 0));
    vec2 dx = dFdx(uv) * size, dy = dFdy(uv) * size;
    float lx = length(dx), ly = length(dy);
    float pMax = max(lx, ly), pMin = max(min(lx, ly), 1e-6);
    float n = clamp(ceil(pMax / pMin), 1.0, maxAniso);
    float lod = max(log2(pMax / n), 0.0);
    vec2 axis = (lx > ly ? dx : dy) / size;
    vec4 sum = vec4(0.0);
    for (float i = 0.0; i < n; i += 1.0) sum += aaTrilinear(t, uv + axis * ((i + 0.5) / n - 0.5), lod);
    return sum / n;
}
