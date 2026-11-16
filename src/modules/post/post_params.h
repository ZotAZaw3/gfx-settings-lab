// C++ mirror of the PostUBO / PassUBO blocks in shaders/post/post_common.glsl (std140, all vec4).
#pragma once

#include <glm/glm.hpp>

namespace gfx::post {

struct PostParams {
    glm::vec4 haze, haze2;
    glm::vec4 dof, dof2, dof3;
    glm::vec4 mb;
    glm::vec4 panini;
    glm::vec4 exposure, exposure2, exposure3;
    glm::vec4 rays, rays2;
    glm::vec4 bloom, bloom2, bloomTint;
    glm::vec4 flare, flare2;
    glm::vec4 glare, glare2;
    glm::vec4 sun, sunRadiance;
    glm::vec4 rain, rain2;
    glm::vec4 tonemap;
    glm::vec4 display;
    glm::vec4 grade, grade2;
    glm::vec4 lift, gamma, gain;
    glm::vec4 ca;
    glm::vec4 vignette, vignetteColor;
    glm::vec4 grain, grain2;
    glm::vec4 letterbox, letterboxColor;
    glm::vec4 calib;
};
static_assert(sizeof(PostParams) % 16 == 0, "std140");

struct PassParams {
    glm::vec4 pass;
};

}  // namespace gfx::post
