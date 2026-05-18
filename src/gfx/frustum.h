#pragma once

#include <glm/glm.hpp>

#include <array>

namespace gfx {

// Axis-aligned bounding box in world space.
struct AABB {
    glm::vec3 min;
    glm::vec3 max;
};

// 6-plane view frustum extracted from a view-projection matrix.
// Plane equation: dot(normal, p) + d >= 0 means "inside / on the plane".
// Stored as vec4(normal.xyz, d) so a single dot4 against (p,1) tests it.
class Frustum {
public:
    // Extract the six planes from a column-major glm::mat4 vp = proj * view.
    // Uses the Gribb-Hartmann method: each plane is a row sum/difference of
    // the matrix rows. Planes are normalized so distance tests are euclidean.
    void from_view_proj(const glm::mat4& vp);

    // True if any part of the AABB is on the inside half-space of all 6
    // planes. False = guaranteed outside. May return true for boxes that
    // are technically outside near a corner (cheap test, no false negatives).
    bool intersects_aabb(const AABB& box) const;

private:
    // Order: left, right, bottom, top, near, far.
    std::array<glm::vec4, 6> planes_{};
};

}  // namespace gfx
