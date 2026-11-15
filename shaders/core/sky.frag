#version 450
// Sky for the forward/MSAA path (SKY_COLOR) and sky motion vectors for the G-buffer (SKY_VELOCITY_ONLY).
#include "core/sky.glsl"
layout(location = 0) in vec2 vUV;
#ifdef SKY_VELOCITY_ONLY
layout(location = 4) out vec2 oVelocity;
void main() { oVelocity = skyVelocity(viewDirFromUv(vUV)); }
#else
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec2 oVelocity;
void main() {
    vec3 dir = viewDirFromUv(vUV);
    oColor = vec4(skyColor(dir), 1.0);
    oVelocity = skyVelocity(dir);
}
#endif
