#pragma once

#include <glad/gl.h>

namespace gfx { class Shader; }

namespace gfx {

// Off-screen HDR pipeline + bloom post-process. The scene draws into a
// floating-point color attachment so the sun and water specular keep
// values >1.0; a separable Gaussian blur extracts and smears bright
// pixels, and an ACES tonemap composites the result to the backbuffer.
class PostProcess {
public:
    PostProcess() = default;
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // Allocates the HDR scene FBO (size w x h) + a half-res bloom chain
    // (two ping-pong FBOs, w/2 x h/2). Safe to call again on resize;
    // existing GL objects are freed first.
    bool init(int w, int h);

    // Bind the scene HDR FBO + set viewport to its size.
    void begin_scene();

    // Run bright extract + 5-tap separable Gaussian blur (configurable
    // iterations) on the scene attachment, then ACES-tonemap composite
    // to the default framebuffer (0). Both shaders are supplied by main
    // so we don't own shader-program lifetime here.
    void resolve_to_backbuffer(const Shader& bright_extract,
                               const Shader& blur,
                               const Shader& tonemap,
                               int backbuffer_w, int backbuffer_h,
                               int blur_iterations = 4,
                               float bloom_threshold = 1.0f,
                               float bloom_intensity = 0.6f,
                               float exposure = 1.0f);

    int width()  const { return w_; }
    int height() const { return h_; }

private:
    void destroy();

    int w_ = 0;
    int h_ = 0;

    GLuint scene_fbo_   = 0;
    GLuint scene_color_ = 0;  // RGBA16F
    GLuint scene_depth_ = 0;  // renderbuffer

    GLuint bloom_fbo_[2]   = {0, 0};
    GLuint bloom_color_[2] = {0, 0};

    // Shared fullscreen-triangle VAO (attributeless, gl_VertexID).
    GLuint fs_vao_ = 0;
};

}  // namespace gfx
