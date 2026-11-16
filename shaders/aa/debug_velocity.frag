#version 450
#include "aa/common_temporal.glsl"
// Visualise motion vectors: hue = direction, brightness = speed (pixels/frame scaled).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 2) uniform sampler2D uVelocity;
layout(std140, binding = 3) uniform DebugParams { vec4 uSize; float uScale; float _p0, _p1, _p2; };
vec3 hsv2rgb(vec3 c) { vec3 p = abs(fract(c.xxx + vec3(1, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0); return c.z * mix(vec3(1), clamp(p - 1.0, 0.0, 1.0), c.y); }
void main() {
    vec2 v = velocityToUV(texture(uVelocity, vUV).rg) * uSize.xy;   // pixels / frame
    float ang = atan(v.y, v.x) / 6.2831853 + 0.5;
    oColor = vec4(hsv2rgb(vec3(ang, 1.0, clamp(length(v) * uScale, 0.0, 1.0))), 1.0);
}
