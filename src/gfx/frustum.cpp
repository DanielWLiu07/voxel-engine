#include "gfx/frustum.h"

namespace gfx {

namespace {

glm::vec4 normalize_plane(const glm::vec4& p) {
    float len = glm::length(glm::vec3(p));
    return (len > 0.0f) ? (p / len) : p;
}

}  // namespace

void Frustum::from_view_proj(const glm::mat4& vp) {
    // glm matrices are column-major: vp[col][row]. Rows of the matrix are
    // accessed by indexing each column at the same row. We need rows r0..r3.
    auto row = [&](int r) {
        return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]);
    };
    const glm::vec4 r0 = row(0);
    const glm::vec4 r1 = row(1);
    const glm::vec4 r2 = row(2);
    const glm::vec4 r3 = row(3);

    planes_[0] = normalize_plane(r3 + r0);  // left
    planes_[1] = normalize_plane(r3 - r0);  // right
    planes_[2] = normalize_plane(r3 + r1);  // bottom
    planes_[3] = normalize_plane(r3 - r1);  // top
    planes_[4] = normalize_plane(r3 + r2);  // near
    planes_[5] = normalize_plane(r3 - r2);  // far
}

bool Frustum::intersects_aabb(const AABB& box) const {
    // For each plane, find the AABB corner farthest along the plane normal
    // (the "p-vertex"). If even that corner is on the negative side of the
    // plane, the entire box is outside that plane and thus outside the
    // frustum.
    for (const auto& p : planes_) {
        glm::vec3 pv;
        pv.x = (p.x >= 0.0f) ? box.max.x : box.min.x;
        pv.y = (p.y >= 0.0f) ? box.max.y : box.min.y;
        pv.z = (p.z >= 0.0f) ? box.max.z : box.min.z;
        if (glm::dot(glm::vec3(p), pv) + p.w < 0.0f) {
            return false;
        }
    }
    return true;
}

}  // namespace gfx
