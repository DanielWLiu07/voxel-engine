#pragma once

#include <glad/gl.h>

#include <cstdint>
#include <span>
#include <string>

namespace gfx {

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    Texture2D(Texture2D&& o) noexcept;
    Texture2D& operator=(Texture2D&& o) noexcept;

    bool load_from_file(const std::string& path);
    // Upload raw RGBA8 pixels (size = w * h * 4)
    void load_from_pixels(std::span<const std::uint8_t> rgba, int w, int h);

    void bind(GLuint unit = 0) const;
    GLuint id() const { return id_; }

private:
    void destroy();

    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace gfx
