#version 450
// Terrain: passes patch corners to the TCS, or (TERRAIN_NO_TESS) displaces a dense grid directly.
#include "geometry/geo_common.glsl"
#include "geometry/terrain_params.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 vPos;
#ifdef TERRAIN_NO_TESS
layout(location = 1) out vec4 vCurClip;
layout(location = 2) out vec4 vPrevClip;
#endif
void main(){
    vPos = aPos;
#ifdef TERRAIN_NO_TESS
    vPos.y = terrainHeight(terrainUV(aPos));
    gl_Position = uViewProj * vec4(vPos, 1.0);
    vCurClip = uUnjitteredViewProj * vec4(vPos, 1.0);
    vPrevClip = uPrevViewProj * vec4(vPos, 1.0);
#endif
}
