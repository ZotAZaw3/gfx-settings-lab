// geometry/decal_common.glsl - mirrors decals.cpp
struct Decal { mat4 model; mat4 invModel; vec4 params; };   // params: kind, age, lifetime, opacity
layout(std430, binding = 8) readonly buffer Decals { Decal decals[]; };
layout(std140, binding = 3) uniform DecalUBO { vec4 dA; };  // x quality (0 albedo, 1 +normal, 2 +roughness)
