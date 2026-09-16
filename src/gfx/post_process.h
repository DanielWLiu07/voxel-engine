#pragma once

#include <glad/gl.h>

namespace gfx { class Shader; }

namespace gfx {

// HDR scene FBO (multisample) + post-process chain (bloom + ACES tonemap).
// Scene renders into a multisample float color attachment so sun/water
// specular keep values >1.0 and edges stay smooth; resolve_to_backbuffer
// blits to a single-sample resolve target, extracts brights, blurs them with
// a downsample/upsample pyramid (dual-filter Kawase), and composites to the
// default framebuffer with ACES + gamma.
class PostProcess {
public:
    PostProcess() = default;
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // samples=1 disables MSAA; 2/4/8 are typical.
    bool init(int w, int h, int samples = 2);

    void begin_scene();

    // sun_uv_x/y are the sun's position in [0,1] screen space and
    // godray_intensity is 0 whenever it is off screen or below the
    // horizon, which is what switches the shaft pass off - the caller owns
    // that decision because it is the only one holding the projection.
    void resolve_to_backbuffer(const Shader& bright_extract,
                               const Shader& bloom_down,
                               const Shader& bloom_up,
                               const Shader& godray,
                               const Shader& tonemap,
                               int backbuffer_w, int backbuffer_h,
                               float bloom_threshold = 1.0f,
                               float bloom_intensity = 0.6f,
                               float exposure = 1.0f,
                               float sun_uv_x = 0.5f,
                               float sun_uv_y = 0.5f,
                               float godray_intensity = 0.0f,
                               float godray_threshold = 1.0f);

    int bloom_mip_count() const { return bloom_mip_count_; }

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

    // Bloom mip chain: level 0 is half-res, each level halves again. The
    // downsample walk fills 0..count-1; the upsample walk accumulates back
    // into level 0, which the tonemap reads as the bloom texture.
    static constexpr int kMaxBloomMips = 7;
    struct BloomMip {
        GLuint fbo = 0;
        GLuint tex = 0;
        int    w   = 0;
        int    h   = 0;
    };
    BloomMip bloom_mips_[kMaxBloomMips];
    int      bloom_mip_count_ = 0;

    // Light shafts, at the same half res as bloom mip 0 and read by the
    // tonemap. A separate target rather than another mip because the
    // upsample walk accumulates back into level 0 and would overwrite it.
    GLuint godray_fbo_   = 0;
    GLuint godray_color_ = 0;
    int    godray_w_     = 0;
    int    godray_h_     = 0;

    GLuint fs_vao_ = 0;
};

}  // namespace gfx
