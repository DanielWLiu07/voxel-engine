#include "game/player.h"

#include "world/block.h"
#include "world/world.h"

#include <cmath>

namespace game {

namespace {

bool aabb_collides(const world::World& w, const glm::vec3& feet) {
    constexpr float half_w = Player::kWidth * 0.5f;
    constexpr float eps = 1e-4f;

    int x0 = static_cast<int>(std::floor(feet.x - half_w));
    int x1 = static_cast<int>(std::floor(feet.x + half_w - eps));
    int y0 = static_cast<int>(std::floor(feet.y));
    int y1 = static_cast<int>(std::floor(feet.y + Player::kHeight - eps));
    int z0 = static_cast<int>(std::floor(feet.z - half_w));
    int z1 = static_cast<int>(std::floor(feet.z + half_w - eps));

    for (int y = y0; y <= y1; ++y)
    for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x)
        if (world::is_solid(w.block_at(x, y, z))) return true;
    return false;
}

// Move along one axis; binary-search to the nearest blocking voxel if hit.
float move_axis(const world::World& w, glm::vec3& pos, int axis, float delta) {
    if (delta == 0.0f) return 0.0f;

    glm::vec3 next = pos;
    next[axis] += delta;
    if (!aabb_collides(w, next)) {
        pos = next;
        return delta;
    }

    float lo = 0.0f;
    float hi = delta;
    for (int i = 0; i < 16; ++i) {
        float mid = 0.5f * (lo + hi);
        glm::vec3 cand = pos;
        cand[axis] += mid;
        if (aabb_collides(w, cand)) hi = mid;
        else                        lo = mid;
    }
    pos[axis] += lo;
    return lo;
}

}  // namespace

void Player::set_position(const glm::vec3& feet) {
    position_ = feet;
    velocity_ = glm::vec3(0.0f);
    on_ground_ = false;
}

void Player::update(const world::World& w, const glm::vec3& wish_horiz,
                    bool jump, float dt) {
    glm::vec3 wish = wish_horiz;
    wish.y = 0.0f;
    velocity_.x = wish.x;
    velocity_.z = wish.z;

    if (jump && on_ground_) {
        velocity_.y = kJumpSpeed;
        on_ground_ = false;
    }
    velocity_.y -= kGravity * dt;

    // Axis-by-axis so a wall on one axis still lets us slide along it.
    move_axis(w, position_, 0, velocity_.x * dt);
    move_axis(w, position_, 2, velocity_.z * dt);

    float dy = velocity_.y * dt;
    float actual_dy = move_axis(w, position_, 1, dy);
    if (dy != 0.0f && std::abs(actual_dy) < std::abs(dy)) {
        if (velocity_.y < 0.0f) on_ground_ = true;
        velocity_.y = 0.0f;
    } else if (dy < 0.0f) {
        on_ground_ = false;
    }
}

}  // namespace game
