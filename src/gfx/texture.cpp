#include "gfx/texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>

namespace gfx {

Texture2D::~Texture2D() { destroy(); }

Texture2D::Texture2D(Texture2D&& o) noexcept
    : id_(o.id_), width_(o.width_), height_(o.height_) {
    o.id_ = 0; o.width_ = o.height_ = 0;
}

Texture2D& Texture2D::operator=(Texture2D&& o) noexcept {
    if (this != &o) {
        destroy();
        id_ = o.id_; width_ = o.width_; height_ = o.height_;
        o.id_ = 0; o.width_ = o.height_ = 0;
    }
    return *this;
}

void Texture2D::destroy() {
    if (id_) { glDeleteTextures(1, &id_); id_ = 0; }
    width_ = height_ = 0;
}

bool Texture2D::load_from_file(const std::string& path) {
    stbi_set_flip_vertically_on_load(1);
    int w = 0, h = 0, ch = 0;
    std::uint8_t* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) {
        std::fprintf(stderr, "[texture] failed to load %s: %s\n",
                     path.c_str(), stbi_failure_reason());
        return false;
    }
    load_from_pixels({data, static_cast<size_t>(w * h * 4)}, w, h);
    stbi_image_free(data);
    return true;
}

void Texture2D::load_from_pixels(std::span<const std::uint8_t> rgba, int w, int h) {
    destroy();
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);

    width_ = w;
    height_ = h;
}

void Texture2D::bind(GLuint unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, id_);
}

}  // namespace gfx
