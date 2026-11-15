#version 450
#include "core/material.glsl"
// Depth-only (shadow maps, prepass). Only alpha-tested materials need a fragment shader body.
void main() {
#ifdef ALPHA_TEST
    GpuMaterial m = currentMaterial();
    if (sampleBaseColor(m).a < m.params.w) discard;
#endif
}
