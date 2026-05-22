#include "gfx/cascaded_shadow_map.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace gfx {

CascadedShadowMap::~CascadedShadowMap() { destroy(); }

bool CascadedShadowMap::init(int size) {
    destroy();
    size_ = size;

    glGenTextures(1, &depth_array_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, depth_array_);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
                 size, size, kNumCascades, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    for (int i = 0; i < kNumCascades; ++i) {
        glGenFramebuffers(1, &fbo_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_[i]);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  depth_array_, 0, i);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[csm] FBO %d incomplete\n", i);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            destroy();
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void CascadedShadowMap::destroy() {
    for (int i = 0; i < kNumCascades; ++i) {
        if (fbo_[i]) { glDeleteFramebuffers(1, &fbo_[i]); fbo_[i] = 0; }
    }
    if (depth_array_) { glDeleteTextures(1, &depth_array_); depth_array_ = 0; }
    size_ = 0;
}

void CascadedShadowMap::begin_pass(int cascade) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_[cascade]);
    glViewport(0, 0, size_, size_);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void CascadedShadowMap::end_pass(int window_w, int window_h) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, window_w, window_h);
}

void CascadedShadowMap::bind_depth_array(GLuint unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, depth_array_);
}

// PSSM split: blend uniform and logarithmic distributions by `lambda`.
static float pssm_split(float n, float f, float i, float N, float lambda) {
    float ratio = i / N;
    float log_ = n * std::pow(f / n, ratio);
    float lin  = n + (f - n) * ratio;
    return lambda * log_ + (1.0f - lambda) * lin;
}

std::array<CascadedShadowMap::Split, kNumCascades>
CascadedShadowMap::fit_cascades(const glm::mat4& camera_view,
                                const glm::mat4& camera_proj,
                                const glm::vec3& light_dir,
                                float near_plane,
                                float far_plane,
                                float lambda) {
    std::array<float, kNumCascades + 1> splits{};
    splits[0] = near_plane;
    splits[kNumCascades] = far_plane;
    for (int i = 1; i < kNumCascades; ++i) {
        splits[i] = pssm_split(near_plane, far_plane,
                               static_cast<float>(i),
                               static_cast<float>(kNumCascades),
                               lambda);
    }

    std::array<Split, kNumCascades> out;
    const glm::mat4 inv_view = glm::inverse(camera_view);
    const glm::vec3 L = glm::normalize(light_dir);

    for (int c = 0; c < kNumCascades; ++c) {
        const float zn = splits[c];
        const float zf = splits[c + 1];

        // 8 corners of this cascade's sub-frustum in view space, then to world.
        glm::mat4 sub_proj = camera_proj;
        // Replace the depth range. Easier: build a fresh perspective with the
        // same FOV/aspect by reading them from the projection matrix.
        // proj[1][1] = cot(fovy/2). proj[0][0] = cot/aspect.
        const float cot_half_fovy = camera_proj[1][1];
        const float fovy = 2.0f * std::atan(1.0f / cot_half_fovy);
        const float aspect = camera_proj[1][1] / camera_proj[0][0];
        sub_proj = glm::perspective(fovy, aspect, zn, zf);

        const glm::mat4 inv_sub_vp = inv_view * glm::inverse(sub_proj);
        glm::vec3 corners_ws[8];
        int idx = 0;
        for (int x : {-1, 1})
        for (int y : {-1, 1})
        for (int z : {-1, 1}) {
            glm::vec4 w = inv_sub_vp * glm::vec4(static_cast<float>(x),
                                                 static_cast<float>(y),
                                                 static_cast<float>(z), 1.0f);
            corners_ws[idx++] = glm::vec3(w) / w.w;
        }

        glm::vec3 center(0.0f);
        for (auto& c2 : corners_ws) center += c2;
        center /= 8.0f;

        // Sphere radius around center — gives a rotation-invariant size and
        // avoids cascade size shimmering as the camera rotates.
        float radius = 0.0f;
        for (auto& c2 : corners_ws) {
            radius = std::max(radius, glm::length(c2 - center));
        }
        radius = std::ceil(radius);

        const glm::vec3 max_ext(radius);
        const glm::vec3 min_ext = -max_ext;

        glm::vec3 up = (std::abs(L.y) > 0.95f) ? glm::vec3(0, 0, 1)
                                                : glm::vec3(0, 1, 0);
        // L points TOWARD the sun, so the light camera sits at center + L*r,
        // looking back at the scene. (Previous - sign was flipped and caused
        // an entire frame of missing shadows.)
        glm::vec3 eye = center + L * radius;
        glm::mat4 lview = glm::lookAt(eye, center, up);
        glm::mat4 lproj = glm::ortho(min_ext.x, max_ext.x,
                                     min_ext.y, max_ext.y,
                                     0.0f, max_ext.z - min_ext.z);

        out[c].light_vp = lproj * lview;
        out[c].split_far_view = zf;
    }
    return out;
}

}  // namespace gfx
