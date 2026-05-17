#include "gfx/camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace gfx {

void FlyCamera::set_yaw_pitch(float yaw_deg, float pitch_deg) {
    yaw_ = yaw_deg;
    pitch_ = std::clamp(pitch_deg, -89.0f, 89.0f);
}

glm::vec3 FlyCamera::forward() const {
    float cy = std::cos(glm::radians(yaw_));
    float sy = std::sin(glm::radians(yaw_));
    float cp = std::cos(glm::radians(pitch_));
    float sp = std::sin(glm::radians(pitch_));
    return glm::normalize(glm::vec3(cy * cp, sp, sy * cp));
}

glm::vec3 FlyCamera::right() const {
    return glm::normalize(glm::cross(forward(), up()));
}

void FlyCamera::apply_mouse_delta(float dx, float dy, float sensitivity) {
    yaw_ += dx * sensitivity;
    pitch_ -= dy * sensitivity;
    pitch_ = std::clamp(pitch_, -89.0f, 89.0f);
}

void FlyCamera::move_local(const glm::vec3& local_dir, float speed, float dt) {
    glm::vec3 f = forward();
    glm::vec3 r = right();
    glm::vec3 u = up();
    glm::vec3 delta = (f * local_dir.z) + (r * local_dir.x) + (u * local_dir.y);
    if (glm::dot(delta, delta) > 0.0f) {
        position_ += glm::normalize(delta) * speed * dt;
    }
}

glm::mat4 FlyCamera::view_matrix() const {
    return glm::lookAt(position_, position_ + forward(), up());
}

glm::mat4 FlyCamera::proj_matrix(float aspect, float fovy_deg,
                                 float znear, float zfar) const {
    return glm::perspective(glm::radians(fovy_deg), aspect, znear, zfar);
}

}  // namespace gfx
