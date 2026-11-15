#include "modules/lighting/LightingShared.h"

namespace gfx::lighting {

Shared& shared() {
    static Shared s;
    return s;
}

glm::mat4 cubeFaceViewProj(int face, glm::vec3 eye, float nearPlane) {
    // forward, right (d dir / du), up (-d dir / dv): see cubeDir() in lighting_common.glsl
    static const glm::vec3 F[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 R[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    static const glm::vec3 U[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    glm::vec3 f = F[face], r = R[face], u = U[face];
    glm::mat4 view(1.0f);   // rows: right, up, -forward (column-major storage)
    view[0][0] = r.x;  view[1][0] = r.y;  view[2][0] = r.z;  view[3][0] = -glm::dot(r, eye);
    view[0][1] = u.x;  view[1][1] = u.y;  view[2][1] = u.z;  view[3][1] = -glm::dot(u, eye);
    view[0][2] = -f.x; view[1][2] = -f.y; view[2][2] = -f.z; view[3][2] = glm::dot(f, eye);
    // 90 degree, aspect 1, infinite reversed-Z, D3D clip: z_clip = near, w_clip = -z_view
    glm::mat4 proj(0.0f);
    proj[0][0] = 1.0f;
    proj[1][1] = 1.0f;
    proj[3][2] = nearPlane;
    proj[2][3] = -1.0f;
    return proj * view;
}

}  // namespace gfx::lighting
