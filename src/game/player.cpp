#include "game/player.h"

#include "world/block.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace game {

namespace {

// Is any voxel inside the player's AABB at `feet` solid?
// AABB spans [feet.x - w/2, feet.x + w/2] on X, [feet.y, feet.y + h] on Y,
// same as X on Z.
bool aabb_collides(const world::World& w, const glm::vec3& feet) {
    constexpr float half_w = Player::kWidth * 0.5f;
    float minx = feet.x - half_w;
    float maxx = feet.x + half_w;
    float miny = feet.y;
    float maxy = feet.y + Player::kHeight;
    float minz = feet.z - half_w;
    float maxz = feet.z + half_w;

    // Test each integer-block cell the AABB overlaps. The std::floor /
    // (maxv - eps) trick avoids re-entering a cell on the high edge.
    constexpr float eps = 1e-4f;
    int x0 = static_cast<int>(std::floor(minx));
    int x1 = static_cast<int>(std::floor(maxx - eps));
    int y0 = static_cast<int>(std::floor(miny));
    int y1 = static_cast<int>(std::floor(maxy - eps));
    int z0 = static_cast<int>(std::floor(minz));
    int z1 = static_cast<int>(std::floor(maxz - eps));

    for (int y = y0; y <= y1; ++y) {
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                if (world::is_solid(w.block_at(x, y, z))) return true;
            }
        }
    }
    return false;
}

// Move along one axis, stop at the nearest blocking voxel. Mutates
// `pos` to the resolved position and returns the displacement we
// actually used (less than `delta` when we hit something).
float move_axis(const world::World& w, glm::vec3& pos, int axis, float delta) {
    if (delta == 0.0f) return 0.0f;

    glm::vec3 next = pos;
    next[axis] += delta;
    if (!aabb_collides(w, next)) {
        pos = next;
        return delta;
    }

    // We collided. Binary-search the largest step that fits. Cheap;
    // happens at most a few times a frame.
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
    // Horizontal velocity is set directly each frame (snappy controls).
    // Vertical velocity persists across frames (gravity + jump).
    glm::vec3 wish = wish_horiz;
    wish.y = 0.0f;
    velocity_.x = wish.x;
    velocity_.z = wish.z;

    if (jump && on_ground_) {
        velocity_.y = kJumpSpeed;
        on_ground_ = false;
    }

    velocity_.y -= kGravity * dt;

    // Move axis-by-axis so collisions on one axis don't block the others
    // (lets you slide along walls).
    move_axis(w, position_, 0, velocity_.x * dt);
    move_axis(w, position_, 2, velocity_.z * dt);

    float dy = velocity_.y * dt;
    float actual_dy = move_axis(w, position_, 1, dy);
    if (dy != 0.0f && std::abs(actual_dy) < std::abs(dy)) {
        // Collided vertically. If we were moving down, we landed.
        if (velocity_.y < 0.0f) on_ground_ = true;
        velocity_.y = 0.0f;
    } else if (dy < 0.0f) {
        on_ground_ = false;
    }
}

}  // namespace game
