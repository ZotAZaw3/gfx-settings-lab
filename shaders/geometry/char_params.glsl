// geometry/char_params.glsl - mirrors CharParams in characters.cpp (std140, UBO 3)
layout(std140, binding = 3) uniform CharUBO {
    mat4 cModel;
    vec4 cA;   // fur: x shell count, y fur length, z density, w gravity      | crowd: x count, y ring center x, z ring center z | cloth: x W, y H
    vec4 cB;   // color rgb, w roughness
    vec4 cC;   // cloth: x dt, y rest length, z iteration mode (0 integrate, 1 constraints), w wind strength
    vec4 cD;   // cloth: xyz collider sphere center, w radius
};
