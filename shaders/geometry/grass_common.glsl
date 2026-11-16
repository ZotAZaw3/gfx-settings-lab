// geometry/grass_common.glsl - mirrors foliage.cpp
struct Blade { vec4 posRot; vec4 params; };   // xyz root, rotation | height, width, bend, color variation
layout(std140, binding = 3) uniform GrassUBO {
    vec4 gA;        // x tier spacing (m), y tier outer half-size (m), z tier inner half-size (m), w tier width scale
    vec4 gB;        // x max distance, y blade segments, z wind, w interaction
    vec4 gC;        // xy snapped grid origin (xz), z grid cells per side, w terrain displacement (m)
    vec4 gD;        // xy terrain center xz, z terrain size, w max blades
    vec4 gPlanes[5];
};
