#version 450
// DoF auto focus: average linear depth around the screen centre, smoothed over time like a focus motor. 1x1 R32F.
#include "post/post_common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(binding = 0) uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D uPrev;
void main() {
    float target = pDof2.x;
    float farClamp = max(uCameraParams.y, 1.0);
    if (pDof2.y > 0.5) {
        float s = 0.0;
        for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++)
            s += min(linearDepth(textureLod(uDepth, vec2(0.5) + vec2(x, y) * 0.01, 0.0).r), farClamp);
        target = s / 25.0;
    }
    float cur = textureLod(uPrev, vec2(0.5), 0.0).r;
    if (!(cur > 0.0) || cur > 1e6) cur = target;
    oColor = vec4(cur + (target - cur) * (1.0 - exp(-frameDt() * pDof3.y)), 0, 0, 1);
}
