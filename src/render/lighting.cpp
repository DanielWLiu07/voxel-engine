#include "render/lighting.h"

#include <cmath>

namespace render {

namespace {

glm::vec3 mix3(const glm::vec3& a, const glm::vec3& b, float t) {
    t = glm::clamp(t, 0.0f, 1.0f);
    return a * (1.0f - t) + b * t;
}

}  // namespace

LightingFrame compute_lighting(float time_of_day) {
    LightingFrame f;

    float sun_angle = (time_of_day - 0.25f) * 6.2831853f;
    f.sun_dir = glm::normalize(glm::vec3(
        std::cos(sun_angle) * 0.3f + 0.05f,
        std::sin(sun_angle),
        std::cos(sun_angle) * 0.6f));
    f.sun_height = f.sun_dir.y;

    const glm::vec3 sun_noon (1.05f, 0.98f, 0.90f);
    const glm::vec3 sun_dusk (1.20f, 0.55f, 0.25f);
    const glm::vec3 sun_night(0.05f, 0.07f, 0.15f);
    const glm::vec3 top_noon (0.30f, 0.55f, 0.85f);
    const glm::vec3 top_dusk (0.18f, 0.20f, 0.40f);
    const glm::vec3 top_night(0.02f, 0.02f, 0.06f);
    const glm::vec3 hz_noon  (0.78f, 0.86f, 0.93f);
    const glm::vec3 hz_dusk  (1.00f, 0.55f, 0.30f);
    const glm::vec3 hz_night (0.05f, 0.06f, 0.10f);
    const glm::vec3 amb_noon (0.32f, 0.34f, 0.40f);
    const glm::vec3 amb_dusk (0.20f, 0.15f, 0.15f);
    const glm::vec3 amb_night(0.05f, 0.06f, 0.10f);

    float day_t   = glm::clamp((f.sun_height - 0.05f) / 0.30f, 0.0f, 1.0f);
    float night_t = glm::clamp(-f.sun_height / 0.15f, 0.0f, 1.0f);

    f.sun_color   = mix3(mix3(sun_night, sun_dusk, 1.0f - night_t), sun_noon, day_t);
    f.sky_top     = mix3(mix3(top_night, top_dusk, 1.0f - night_t), top_noon, day_t);
    f.sky_horizon = mix3(mix3(hz_night,  hz_dusk,  1.0f - night_t), hz_noon,  day_t);
    f.ambient     = mix3(mix3(amb_night, amb_dusk, 1.0f - night_t), amb_noon, day_t);

    // Below the horizon: still light scene from slightly above so the
    // ground isn't lit from underneath.
    f.light_dir = (f.sun_height > 0.0f)
        ? f.sun_dir
        : glm::normalize(glm::vec3(f.sun_dir.x * 0.2f, 0.4f, f.sun_dir.z * 0.2f));

    f.shadow_strength = glm::clamp(f.sun_height * 4.0f, 0.0f, 1.0f);

    return f;
}

}  // namespace render
