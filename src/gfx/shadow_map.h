#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

namespace gfx {

// Depth-only framebuffer used for directional-light shadow casting.
// One texture, no color attachment. Caller binds it, renders the scene
// from the light's POV with a cheap depth shader, then samples in the
// main pass.
class ShadowMap {
public:
    ShadowMap() = default;
    ~ShadowMap();
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    bool init(int size);

    // Bind the FBO + set viewport to the depth texture's dimensions.
    void begin_pass();
    // Restore the default framebuffer + the supplied window viewport.
    void end_pass(int window_w, int window_h);

    // Sample binding for the main pass.
    void bind_depth_texture(GLuint unit) const;

    GLuint depth_texture() const { return depth_tex_; }
    int    size() const { return size_; }

    // Build an orthographic light view-projection covering an area of
    // radius `world_radius` blocks around `center`, looking from
    // `light_dir` toward the center.
    static glm::mat4 fit_view_proj(const glm::vec3& center,
                                   const glm::vec3& light_dir,
                                   float world_radius,
                                   float depth_extent);

private:
    void destroy();
    GLuint fbo_ = 0;
    GLuint depth_tex_ = 0;
    int size_ = 0;
};

}  // namespace gfx
