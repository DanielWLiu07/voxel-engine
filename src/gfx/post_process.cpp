#include "gfx/post_process.h"

#include "core/profiler.h"
#include "gfx/shader.h"

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
                                        const Shader& tonemap,
                                        int backbuffer_w, int backbuffer_h,
                                        float bloom_threshold,
                                        float bloom_intensity,
                                        float exposure) {
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
    tonemap.set_float("u_exposure", exposure);
    tonemap.set_float("u_bloom_intensity", bloom_intensity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resolve_color_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloom_mips_[0].tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

}  // namespace gfx
