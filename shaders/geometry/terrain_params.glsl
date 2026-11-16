// geometry/terrain_params.glsl - mirrors TerrainParams in src/modules/geometry/terrain.cpp (std140, UBO 3)
layout(std140, binding = 3) uniform TerrainUBO {
    vec4 tA;   // x terrain size (m), y displacement height (m), z target triangle size (px), w max tess level
    vec4 tB;   // x proj scale (px per unit at distance 1), y cull patches, z material layers, w triplanar mode
    vec4 tC;   // xy terrain center (world xz), z show-triangles debug, w unused
};
layout(binding = 0) uniform sampler2D tHeightMap;   // R32F normalized height (x tA.y = metres)
vec2  terrainUV(vec3 p) { return (p.xz - tC.xy) / tA.x + 0.5; }
float terrainHeight(vec2 uv) { return textureLod(tHeightMap, uv, 0.0).r * tA.y; }
