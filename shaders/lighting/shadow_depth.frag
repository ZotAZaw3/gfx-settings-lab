#version 450
// shadow_depth.frag - depth comes from the rasterizer. ALPHA_TEST variant clips foliage cards so
// leaves cast leaf-shaped shadows instead of quads.
#include "common/draw.glsl"
layout(location = 0) in vec2 vUV;
void main() {
#ifdef ALPHA_TEST
    GpuMaterial m = materials[uIds.x];
    float a = m.baseColor.a * texture(tBaseColor, vUV).a;
    if (a < m.params.w) discard;
#endif
}
