#pragma once

#include <glm/glm.hpp>

namespace world { class World; }

namespace game {

// AABB player with swept collision and gravity. Position is the feet
// (bottom-center); eye sits near the top of the hitbox.
class Player {
public:
    void set_position(const glm::vec3& feet);
    glm::vec3 feet_position() const { return position_; }
    glm::vec3 eye_position()  const { return position_ + glm::vec3(0.0f, kEyeHeight, 0.0f); }
    glm::vec3 velocity()      const { return velocity_; }
    bool      on_ground()     const { return on_ground_; }

    void update(const world::World& w, const glm::vec3& wish_horiz,
                bool jump, float dt);

    static constexpr float kWidth       = 0.6f;
    static constexpr float kHeight      = 1.8f;
    static constexpr float kEyeHeight   = 1.65f;
    static constexpr float kWalkSpeed   = 5.0f;
    static constexpr float kSprintSpeed = 9.0f;
    static constexpr float kJumpSpeed   = 8.4f;
    static constexpr float kGravity     = 28.0f;

private:
    glm::vec3 position_{0.0f};
    glm::vec3 velocity_{0.0f};
    bool      on_ground_ = false;
};

}  // namespace game
