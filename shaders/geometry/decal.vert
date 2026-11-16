#version 450
// Instanced unit cube [-0.5,0.5]^3 per decal (projects along its local -Y onto whatever is inside).
#include "geometry/geo_common.glsl"
#include "geometry/decal_common.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 0) flat out int vIndex;
layout(location = 1) flat out vec3 vFaceN;
layout(location = 2) out vec3 vWorld;
void main(){
    Decal d = decals[gl_InstanceID];
    vec4 w = d.model * vec4(aPos, 1.0);
    vIndex = gl_InstanceID;
    vFaceN = normalize(mat3(d.model) * aNormal);
    vWorld = w.xyz;
    gl_Position = uViewProj * w;
}
