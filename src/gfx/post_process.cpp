#include "gfx/post_process.h"

#include "core/profiler.h"
#include "gfx/shader.h"

#include <glm/vec2.hpp>

#include <algorithm>
#include <cstdio>

namespace gfx {

namespace {

GLuint make_color_texture(int w, int h, GLenum internal_format) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

GLuint make_ms_color_texture(int w, int h, int samples, GLenum internal_format) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, tex);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples,
                            internal_format, w, h, GL_TRUE);
    return tex;
}

}  // namespace

PostProcess::~PostProcess() { destroy(); }

void PostProcess::destroy() {
    if (scene_fbo_)      { glDeleteFramebuffers(1, &scene_fbo_);      scene_fbo_ = 0; }
    if (scene_color_ms_) { glDeleteTextures(1, &scene_color_ms_);     scene_color_ms_ = 0; }
    if (scene_depth_ms_) { glDeleteRenderbuffers(1, &scene_depth_ms_); scene_depth_ms_ = 0; }
    if (resolve_fbo_)    { glDeleteFramebuffers(1, &resolve_fbo_);    resolve_fbo_ = 0; }
    if (resolve_color_)  { glDeleteTextures(1, &resolve_color_);      resolve_color_ = 0; }
    for (int i = 0; i < kMaxBloomMips; ++i) {
        if (bloom_mips_[i].fbo) { glDeleteFramebuffers(1, &bloom_mips_[i].fbo); }
        if (bloom_mips_[i].tex) { glDeleteTextures(1, &bloom_mips_[i].tex); }
        bloom_mips_[i] = BloomMip{};
    }
    bloom_mip_count_ = 0;
    if (godray_fbo_)   { glDeleteFramebuffers(1, &godray_fbo_);  godray_fbo_ = 0; }
    if (godray_color_) { glDeleteTextures(1, &godray_color_);    godray_color_ = 0; }
    godray_w_ = godray_h_ = 0;
    if (fs_vao_) { glDeleteVertexArrays(1, &fs_vao_); fs_vao_ = 0; }
    w_ = h_ = 0;
    samples_ = 1;
}

bool PostProcess::init(int w, int h, int samples) {
    destroy();
    w_ = w;
    h_ = h;
    samples_ = std::max(1, samples);

    // Scene HDR FBO (multisample when samples_ > 1).
    if (samples_ > 1) {
        scene_color_ms_ = make_ms_color_texture(w, h, samples_, GL_RGBA16F);
        glGenRenderbuffers(1, &scene_depth_ms_);
        glBindRenderbuffer(GL_RENDERBUFFER, scene_depth_ms_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples_,
                                         GL_DEPTH_COMPONENT24, w, h);
    } else {
        scene_color_ms_ = make_color_texture(w, h, GL_RGBA16F);
        glGenRenderbuffers(1, &scene_depth_ms_);
        glBindRenderbuffer(GL_RENDERBUFFER, scene_depth_ms_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    }

    glGenFramebuffers(1, &scene_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    if (samples_ > 1) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D_MULTISAMPLE, scene_color_ms_, 0);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, scene_color_ms_, 0);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, scene_depth_ms_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[postfx] scene FBO incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy();
        return false;
    }

    // Single-sample resolve target. post-process samples from this.
    resolve_color_ = make_color_texture(w, h, GL_RGBA16F);
    glGenFramebuffers(1, &resolve_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, resolve_color_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[postfx] resolve FBO incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy();
        return false;
    }

    // Bloom mip chain. Level 0 is half-res; each level halves again until the
    // smaller side drops to ~8 px or we hit the cap. More, smaller levels =
    // wider blur, but the geometric shrink keeps total pixel work tiny.
    int mw = std::max(1, w / 2);
    int mh = std::max(1, h / 2);
    bloom_mip_count_ = 0;
    for (int i = 0; i < kMaxBloomMips; ++i) {
        BloomMip& mip = bloom_mips_[i];
        mip.w = mw;
        mip.h = mh;
        mip.tex = make_color_texture(mw, mh, GL_RGBA16F);
        glGenFramebuffers(1, &mip.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, mip.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, mip.tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[postfx] bloom mip %d incomplete\n", i);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            destroy();
            return false;
        }
        ++bloom_mip_count_;
        if (mw <= 16 || mh <= 16) break;
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
    }

    // Light shafts, at mip 0's resolution.
    //
    // Half rather than quarter, and that is a measurement rather than a
    // default. Quartering the axes - a sixteenth of the pixels - did not
    // make the pass cheaper: ABBA-paired 600-frame runs put it at +2.15 ms
    // against +2.30 ms at half, which is inside the spread. The cost is
    // not the pixels it writes, it is the 32 taps each one makes into a
    // full-resolution RGBA16F scene target, and a smaller output makes
    // those taps land FURTHER apart, trading fewer of them for worse
    // locality. Cutting the taps instead (32 to 4, at quarter res) took
    // it to +0.98 ms, which is the other half of the same story.
    //
    // So: half res, because it costs what quarter costs and looks better.
    godray_w_ = bloom_mips_[0].w;
    godray_h_ = bloom_mips_[0].h;
    godray_color_ = make_color_texture(godray_w_, godray_h_, GL_RGBA16F);
    glGenFramebuffers(1, &godray_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, godray_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, godray_color_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[postfx] godray target incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy();
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenVertexArrays(1, &fs_vao_);
    return true;
}

void PostProcess::begin_scene() {
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glViewport(0, 0, w_, h_);
}

void PostProcess::resolve_to_backbuffer(const Shader& bright_extract,
                                        const Shader& bloom_down,
                                        const Shader& bloom_up,
                                        const Shader& godray,
                                        const Shader& tonemap,
                                        int backbuffer_w, int backbuffer_h,
                                        float bloom_threshold,
                                        float bloom_intensity,
                                        float exposure,
                                        float sun_uv_x,
                                        float sun_uv_y,
                                        float godray_intensity,
                                        float godray_threshold) {
    ZoneScopedN("postfx_resolve");
    // MSAA resolve: blit multisample scene -> single-sample resolve target.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo_);
    glBlitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindVertexArray(fs_vao_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    // Bright extract with soft-knee threshold: resolve -> mip 0 (half res).
    bright_extract.use();
    bright_extract.set_int("u_scene", 0);
    bright_extract.set_float("u_threshold", bloom_threshold);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, bloom_mips_[0].fbo);
    glViewport(0, 0, bloom_mips_[0].w, bloom_mips_[0].h);
    glBindTexture(GL_TEXTURE_2D, resolve_color_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Light shafts, marched over the resolved scene with a threshold of
    // their own. Reusing the bloom's bright-extract was the first design
    // and it does not work - see godray.frag for the measurement.
    //
    // Skipped outright when the sun is not contributing, which is most of
    // the night and any frame facing away from it. The target still holds
    // the last frame's shafts then, so the tonemap's own intensity is
    // what zeroes them - clearing here would be a second full-target
    // write to accomplish what a multiply by zero already does.
    if (godray_intensity > 0.0f) {
        godray.use();
        godray.set_int("u_scene", 0);
        godray.set_vec2("u_sun_uv", glm::vec2(sun_uv_x, sun_uv_y));
        // Walked over three quarters of the distance to the sun, decaying
        // to about a fifth across the march. Longer reads as fog, shorter
        // reads as a halo.
        godray.set_float("u_density", 0.75f);
        godray.set_float("u_decay", 0.968f);
        godray.set_float("u_weight", 0.55f);
        // Set by the caller from the sky it just lit, so only what is
        // brighter than the ambient sky emits.
        godray.set_float("u_threshold", godray_threshold);
        glBindFramebuffer(GL_FRAMEBUFFER, godray_fbo_);
        glViewport(0, 0, godray_w_, godray_h_);
        glBindTexture(GL_TEXTURE_2D, resolve_color_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Downsample walk: mip[i-1] -> mip[i], halving each step.
    bloom_down.use();
    bloom_down.set_int("u_source", 0);
    for (int i = 1; i < bloom_mip_count_; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, bloom_mips_[i].fbo);
        glViewport(0, 0, bloom_mips_[i].w, bloom_mips_[i].h);
        glBindTexture(GL_TEXTURE_2D, bloom_mips_[i - 1].tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Upsample walk: mip[i] -> mip[i-1], composited additively so each level
    // adds a successively wider halo. Result accumulates into mip 0.
    bloom_up.use();
    bloom_up.set_int("u_source", 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    for (int i = bloom_mip_count_ - 1; i > 0; --i) {
        glBindFramebuffer(GL_FRAMEBUFFER, bloom_mips_[i - 1].fbo);
        glViewport(0, 0, bloom_mips_[i - 1].w, bloom_mips_[i - 1].h);
        glBindTexture(GL_TEXTURE_2D, bloom_mips_[i].tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDisable(GL_BLEND);

    // Tonemap composite -> default framebuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, backbuffer_w, backbuffer_h);
    tonemap.use();
    tonemap.set_int("u_scene", 0);
    tonemap.set_int("u_bloom", 1);
    tonemap.set_int("u_godray", 2);
    tonemap.set_float("u_exposure", exposure);
    tonemap.set_float("u_bloom_intensity", bloom_intensity);
    tonemap.set_float("u_godray_intensity", godray_intensity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolve_color_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloom_mips_[0].tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, godray_color_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

}  // namespace gfx
