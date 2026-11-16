// ss_trace.glsl - screen-space ray marcher shared by SSR and SSGI (docs/reflections.md).
// Marches in (uv, ndcZ) space, where NDC depth is affine along a screen-space line.
// Reversed-Z: a larger depth is closer. The ray is BEHIND the surface when rayZ < sceneZ.
#ifndef SS_TRACE_GLSL
#define SS_TRACE_GLSL
layout(binding = 7) uniform sampler2D uHiZ;   // closest-depth pyramid, mip 0 = depth

// Returns true on hit; hitUV receives the hit position.
// mode 0 = linear march with binary refinement, 1 = HiZ accelerated.
bool traceScreen(vec3 originVS, vec3 dirVS, int steps, float thicknessVS, float jitter, int mode, out vec2 hitUV) {
    hitUV = vec2(0);
    // clip the ray so the end point stays in front of the near plane
    float maxDist = 100.0;
    if (dirVS.z > 0.0) maxDist = min(maxDist, (-uCameraPos.w * 1.01 - originVS.z) / dirVS.z);
    vec3 endVS = originVS + dirVS * maxDist;
    vec3 a = projectView(originVS), b = projectView(endVS);
    // clip end to the screen rectangle in uv
    vec2 dUV = b.xy - a.xy;
    float tMax = 1.0;
    for (int k = 0; k < 2; ++k) {
        if (dUV[k] > 0.0) tMax = min(tMax, (1.0 - a[k]) / dUV[k]);
        else if (dUV[k] < 0.0) tMax = min(tMax, (0.0 - a[k]) / dUV[k]);
    }
    b = mix(a, b, tMax);
    vec3 delta = b - a;
    float lenPx = length(delta.xy * uRenderSize.xy);
    if (lenPx < 1.0) return false;

    if (mode == 0) {
        float dt = 1.0 / float(steps);
        float t = dt * jitter;
        float prevT = 0.0;
        for (int i = 0; i < steps; ++i) {
            t += dt;
            vec3 p = a + delta * t;
            float sceneZ = textureLod(uHiZ, p.xy, 0.0).r;
            if (p.z < sceneZ) {
                // thickness test in view space
                float rayV = viewPosFromDepth(p.xy, max(p.z, 1e-7)).z;
                float sceneV = viewPosFromDepth(p.xy, max(sceneZ, 1e-7)).z;
                if (sceneV - rayV > thicknessVS) { prevT = t; continue; }
                // binary refine
                float lo = prevT, hi = t;
                for (int r = 0; r < 5; ++r) {
                    float m = 0.5 * (lo + hi);
                    vec3 q = a + delta * m;
                    if (q.z < textureLod(uHiZ, q.xy, 0.0).r) hi = m; else lo = m;
                }
                hitUV = (a + delta * hi).xy;
                return true;
            }
            prevT = t;
        }
        return false;
    }

    // HiZ: advance by the current level's cell size, climb while free, descend on contact
    int maxLevel = textureQueryLevels(uHiZ) - 1;
    float pxStep = 1.0 / lenPx;          // t per full-res pixel
    float t = pxStep * (1.0 + jitter);
    int level = 0;
    for (int i = 0; i < steps * 2 && t <= 1.0; ++i) {
        float tn = t + pxStep * float(1 << level);
        vec3 p = a + delta * min(tn, 1.0);
        float cellZ = textureLod(uHiZ, p.xy, float(level)).r;
        if (p.z >= cellZ) {                    // in front of everything in this cell
            t = tn;
            level = min(level + 1, maxLevel);
        } else if (level > 0) {
            level--;
        } else {
            float rayV = viewPosFromDepth(p.xy, max(p.z, 1e-7)).z;
            float sceneV = viewPosFromDepth(p.xy, max(cellZ, 1e-7)).z;
            if (sceneV - rayV > thicknessVS) { t = tn; continue; }
            hitUV = p.xy;
            return true;
        }
    }
    return false;
}
#endif
