#version 450
// Shell fur: the mesh is drawn once per shell (instance), pushed out along the normal; outer shells droop.
#include "geometry/geo_common.glsl"
#include "geometry/char_params.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec2 aUV;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vShell;
layout(location = 3) out vec4 vCurClip;
layout(location = 4) out vec4 vPrevClip;
void main(){
    float t = (cA.x > 0.0) ? float(gl_InstanceID) / cA.x : 0.0;
    vec3 n = normalize(mat3(cModel) * aNormal);
    vec4 w = cModel * vec4(aPos, 1.0);
    float wind = sin(uTime.x * 2.0 + w.x * 0.8) * 0.15;
    vec3 p = w.xyz + n * cA.y * t + (vec3(wind, -cA.w, 0.0)) * cA.y * t * t;
    vNormal = n; vUV = aUV; vShell = t;
    gl_Position = uViewProj * vec4(p, 1.0);
    vCurClip = uUnjitteredViewProj * vec4(p, 1.0);
    vPrevClip = uPrevViewProj * vec4(p, 1.0);
}
