#include "gfx/frustum.h"

namespace gfx {

namespace {

glm::vec4 normalize_plane(const glm::vec4& p) {
    float len = glm::length(glm::vec3(p));
    return (len > 0.0f) ? (p / len) : p;
}

}  // namespace

// Gribb-Hartmann: each plane is a row sum/difference of vp's rows.
void Frustum::from_view_proj(const glm::mat4& vp) {
    auto row = [&](int r) {
        return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]);
    };
    const glm::vec4 r0 = row(0);
    const glm::vec4 r1 = row(1);
    const glm::vec4 r2 = row(2);
    const glm::vec4 r3 = row(3);

    planes_[0] = normalize_plane(r3 + r0);
    planes_[1] = normalize_plane(r3 - r0);
    planes_[2] = normalize_plane(r3 + r1);
    planes_[3] = normalize_plane(r3 - r1);
    planes_[4] = normalize_plane(r3 + r2);
    planes_[5] = normalize_plane(r3 - r2);
}

// P-vertex test: pick the AABB corner farthest along each plane's normal.
// If even that corner is outside, the whole box is outside.
bool Frustum::intersects_aabb(const AABB& box) const {
    for (const auto& p : planes_) {
        glm::vec3 pv;
        pv.x = (p.x >= 0.0f) ? box.max.x : box.min.x;
        pv.y = (p.y >= 0.0f) ? box.max.y : box.min.y;
        pv.z = (p.z >= 0.0f) ? box.max.z : box.min.z;
        if (glm::dot(glm::vec3(p), pv) + p.w < 0.0f) return false;
    }
    return true;
}

}  // namespace gfx
