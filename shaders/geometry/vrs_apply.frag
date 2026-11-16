#version 450
// Software VRS emulation: a coarse tile keeps one shaded sample per 2x2 / 4x4 block (broadcast),
// which is exactly what hardware VRS does to the pixel shader. Shows the visual cost; the GPU
// saving needs hardware VRS (D3D12 / Vulkan / GL_NV_shading_rate_image), not available via rhi.
#include "common/frame.glsl"
layout(std140, binding = 3) uniform VrsUBO { vec4 vrA; };
layout(binding = 0) uniform sampler2D tColorCopy;
layout(binding = 1) uniform usampler2D tRate;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main(){
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 sz = textureSize(tColorCopy, 0);
    uint rate = texelFetch(tRate, min(p / 16, textureSize(tRate, 0) - 1), 0).r;
    int block = 1 << int(rate);
    ivec2 anchor = min((p / block) * block + block / 2, sz - 1);
    vec3 c = texelFetch(tColorCopy, anchor, 0).rgb;
    if (vrA.w > 0.5){
        if (rate == 1u) c = mix(c, vec3(0.1, 0.8, 0.2) * max(luminance(c), 0.2), 0.35);
        if (rate == 2u) c = mix(c, vec3(0.9, 0.15, 0.1) * max(luminance(c), 0.2), 0.35);
        ivec2 t = p % 16; if (t.x == 0 || t.y == 0) c *= 0.7;  // tile grid
    }
    outColor = vec4(c, 1.0);
}
