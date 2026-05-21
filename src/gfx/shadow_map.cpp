#include "gfx/shadow_map.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>

namespace gfx {

ShadowMap::~ShadowMap() { destroy(); }

bool ShadowMap::init(int size) {
    destroy();
    size_ = size;

    glGenTextures(1, &depth_tex_);
    glBindTexture(GL_TEXTURE_2D, depth_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    // sampler2DShadow path: HW does the compare + 2x2 bilinear blend.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, depth_tex_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[shadow] FBO incomplete: 0x%X\n", status);
        destroy();
        return false;
    }
    return true;
}

void ShadowMap::destroy() {
    if (fbo_) { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (depth_tex_) { glDeleteTextures(1, &depth_tex_); depth_tex_ = 0; }
    size_ = 0;
}

void ShadowMap::begin_pass() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, size_, size_);
    // Defensive: another pass may have left depth writes off.
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void ShadowMap::end_pass(int window_w, int window_h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, window_w, window_h);
}

void ShadowMap::bind_depth_texture(GLuint unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, depth_tex_);
}

glm::mat4 ShadowMap::fit_view_proj(const glm::vec3& center,
                                   const glm::vec3& light_dir,
                                   float world_radius,
                                   float depth_extent) {
    glm::vec3 L = glm::normalize(light_dir);
    glm::vec3 up = (std::abs(L.y) > 0.95f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    glm::vec3 eye = center + L * depth_extent;
    glm::mat4 view = glm::lookAt(eye, center, up);
    glm::mat4 proj = glm::ortho(-world_radius, world_radius,
                                -world_radius, world_radius,
                                0.0f, depth_extent * 2.0f);
    return proj * view;
}

}  // namespace gfx
