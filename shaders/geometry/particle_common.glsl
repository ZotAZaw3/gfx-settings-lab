// geometry/particle_common.glsl - particle layout + per-emitter params (mirrors particles.cpp)
struct Particle { vec4 posLife; vec4 velAge; vec4 color; };   // xyz,life | xyz,age | rgb,size
layout(std140, binding = 3) uniform EmitterUBO {
    vec4 eA;   // xyz emitter position, w type (0 sparks, 1 smoke, 2 fire)
    vec4 eB;   // x active count, y base index, z dt, w time
    vec4 eC;   // x collision, y soft particles, z soft distance (m), w lit
    vec4 eD;   // x flipbook blend, y manual depth test, zw 1 / target size
};
