#pragma once

#include <glad/gl.h>

namespace gfx { class Shader; }

namespace gfx {

// HDR scene FBO (multisample) + post-process chain (bloom + ACES tonemap).
// Scene renders into a multisample float color attachment so sun/water
// specular keep values >1.0 and edges stay smooth; resolve_to_backbuffer
// blits to a single-sample resolve target, extracts brights, blurs them
// separably, and composites to the default framebuffer with ACES + gamma.
class PostProcess {
public:
    PostProcess() = default;
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // samples=1 disables MSAA; 2/4/8 are typical.
    bool init(int w, int h, int samples = 2);

    void begin_scene();

    void resolve_to_backbuffer(const Shader& bright_extract,
                               const Shader& blur,
                               const Shader& tonemap,
                               int backbuffer_w, int backbuffer_h,
                               int blur_iterations = 4,
                               float bloom_threshold = 1.0f,
                               float bloom_intensity = 0.6f,
                               float exposure = 1.0f);

    int width()   const { return w_; }
    int height()  const { return h_; }
    int samples() const { return samples_; }

private:
    void destroy();

    int w_ = 0;
    int h_ = 0;
    int samples_ = 1;

    // Multisample scene target (rendered into).
    GLuint scene_fbo_      = 0;
    GLuint scene_color_ms_ = 0;  // RGBA16F multisample texture
    GLuint scene_depth_ms_ = 0;  // depth renderbuffer (multisample if samples>1)

    // Single-sample resolve target (sampled from in post pass).
    GLuint resolve_fbo_   = 0;
    GLuint resolve_color_ = 0;  // RGBA16F texture

    GLuint bloom_fbo_[2]   = {0, 0};
    GLuint bloom_color_[2] = {0, 0};

    GLuint fs_vao_ = 0;
};

}  // namespace gfx
