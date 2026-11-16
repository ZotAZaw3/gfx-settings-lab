#version 450
// Downsample reversed-Z depth by MAX (= nearest) for reduced-resolution effects. Factor in UBO 3.
layout(std140, binding = 3) uniform DownUBO { ivec4 dFactor; };
layout(binding = 0) uniform sampler2D tSrcDepth;
layout(location = 0) in vec2 vUV;
layout(location = 0) out float outDepth;
void main(){
    int f = dFactor.x;
    ivec2 base = ivec2(gl_FragCoord.xy) * f; ivec2 sz = textureSize(tSrcDepth, 0) - 1;
    float m = 0.0;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x){
        if (x >= f || y >= f) continue;
        m = max(m, texelFetch(tSrcDepth, min(base + ivec2(x, y), sz), 0).r);
    }
    outDepth = m;
}
