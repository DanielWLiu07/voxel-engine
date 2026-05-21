#pragma once

#include <glm/glm.hpp>

#include <array>

namespace gfx {

struct AABB {
    glm::vec3 min;
    glm::vec3 max;
};

// 6 planes extracted from a view-proj. Plane: dot(n, p) + d >= 0 = inside.
class Frustum {
public:
    void from_view_proj(const glm::mat4& vp);

    // Cheap test: true if any AABB corner could be inside. No false negatives.
    bool intersects_aabb(const AABB& box) const;

private:
    // left, right, bottom, top, near, far
    std::array<glm::vec4, 6> planes_{};
};

}  // namespace gfx
