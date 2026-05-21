#pragma once

#include <glad/gl.h>

#include <cstddef>

namespace gfx {

// Subdivided XZ quad. CPU stores position only; vertex shader sets Y to
// sea level + wave displacement so the grid resolution sets ripple quality.
class WaterPlane {
public:
    WaterPlane() = default;
    ~WaterPlane();
    WaterPlane(const WaterPlane&) = delete;
    WaterPlane& operator=(const WaterPlane&) = delete;

    bool init(float size_blocks, int segments);
    void draw() const;

    std::size_t index_count() const { return index_count_; }

private:
    void destroy();
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    std::size_t index_count_ = 0;
};

}  // namespace gfx
