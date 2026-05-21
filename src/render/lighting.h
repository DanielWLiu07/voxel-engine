#pragma once

#include <glm/glm.hpp>

namespace render {

// Output of the day/night sampler. Shared between the sky shader, the
// terrain shader, the shadow pass, and the water shader so they all
// agree on sun direction and atmosphere color.
struct LightingFrame {
    glm::vec3 sun_dir;          // points toward the sun, normalized
    glm::vec3 sun_color;
    glm::vec3 sky_top;
    glm::vec3 sky_horizon;
    glm::vec3 ambient;
    float     sun_height;       // sun_dir.y, in [-1, 1]
    float     shadow_strength;  // 0 below horizon, ramps to 1 above
    glm::vec3 light_dir;        // sun_dir, or slightly-up below horizon
};

// time_of_day in [0, 1): 0.25 sunrise, 0.5 noon, 0.75 sunset, 0 midnight.
LightingFrame compute_lighting(float time_of_day);

}  // namespace render
