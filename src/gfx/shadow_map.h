#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

namespace gfx {

// Depth-only FBO for directional-light shadows.
class ShadowMap {
public:
    ShadowMap() = default;
    ~ShadowMap();
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    bool init(int size);

    void begin_pass();
    void end_pass(int window_w, int window_h);
    void bind_depth_texture(GLuint unit) const;

    GLuint depth_texture() const { return depth_tex_; }
    int    size() const { return size_; }

    // Orthographic light view-proj covering `world_radius` around center.
    static glm::mat4 fit_view_proj(const glm::vec3& center,
                                   const glm::vec3& light_dir,
                                   float world_radius,
                                   float depth_extent);

private:
    void destroy();
    GLuint fbo_ = 0;
    GLuint depth_tex_ = 0;
    int    size_ = 0;
};

}  // namespace gfx
