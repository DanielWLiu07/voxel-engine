#pragma once

#include <glm/glm.hpp>

namespace gfx {

class FlyCamera {
public:
    FlyCamera() = default;

    void set_position(const glm::vec3& p) { position_ = p; }
    glm::vec3 position() const { return position_; }

    void  set_yaw_pitch(float yaw_deg, float pitch_deg);
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

    glm::vec3 forward() const;
    glm::vec3 right() const;
    glm::vec3 up() const { return {0.0f, 1.0f, 0.0f}; }

    void apply_mouse_delta(float dx, float dy, float sensitivity);
    void move_local(const glm::vec3& local_dir, float speed, float dt);

    glm::mat4 view_matrix() const;
    glm::mat4 proj_matrix(float aspect, float fovy_deg = 70.0f,
                          float znear = 0.1f, float zfar = 500.0f) const;

private:
    glm::vec3 position_{0.0f, 0.0f, 5.0f};
    float yaw_ = -90.0f;
    float pitch_ = 0.0f;
};

}  // namespace gfx
