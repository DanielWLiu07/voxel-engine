#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <array>

namespace gfx {

inline constexpr int kNumCascades = 3;

// Depth-only FBO array for parallel-split (cascaded) shadow mapping.
// One layer per cascade in a GL_TEXTURE_2D_ARRAY. Each layer is sampled
// with sampler2DArrayShadow so hardware PCF still applies.
class CascadedShadowMap {
public:
    CascadedShadowMap() = default;
    ~CascadedShadowMap();
    CascadedShadowMap(const CascadedShadowMap&) = delete;
    CascadedShadowMap& operator=(const CascadedShadowMap&) = delete;

    bool init(int size);

    void begin_pass(int cascade);
    void end_pass(int window_w, int window_h);
    void bind_depth_array(GLuint unit) const;

    GLuint depth_array_texture() const { return depth_array_; }
    int    size() const { return size_; }

    // Build per-cascade light view-proj matrices that tightly fit the
    // camera's sub-frustum between split_near and split_far. Output is
    // 3 VPs (one per cascade) and the matching 3 split-far values in
    // view space (positive depth) for the shader's cascade selector.
    struct Split {
        glm::mat4 light_vp;
        float     split_far_view; // distance along camera forward
    };
    static std::array<Split, kNumCascades> fit_cascades(
        const glm::mat4& camera_view,
        const glm::mat4& camera_proj,
        const glm::vec3& light_dir,
        float near_plane,
        float far_plane,
        float lambda = 0.5f);

private:
    void destroy();
    GLuint fbo_[kNumCascades]{};
    GLuint depth_array_ = 0;
    int    size_ = 0;
};

}  // namespace gfx
