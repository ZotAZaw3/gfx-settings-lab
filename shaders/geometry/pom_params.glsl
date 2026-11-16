// geometry/pom_params.glsl - mirrors PomParams in terrain.cpp (std140, UBO 3)
layout(std140, binding = 3) uniform PomUBO {
    mat4 pModel;
    vec4 pT, pB, pN;      // world tangent frame of the panel (xyz)
    vec4 pP0;             // x mode (0 normal map, 1 POM), y depth scale, z min layers, w max layers
    vec4 pP1;             // x binary refine steps, y self-shadow steps, z uv tiling, w unused
};
