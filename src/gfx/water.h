#pragma once

#include <glad/gl.h>

#include <cstddef>

namespace gfx {

// A flat, subdivided XZ quad rendered as the world's water surface. The
// CPU only stores position; the vertex shader fixes Y to sea level and
// adds a small wave displacement so we get visible ripples without a
// separate normal/uv stream.
//
// Subdivision matters: per-vertex sine displacement only produces nice
// waves if the grid is fine enough. ~1 vertex per 2 blocks is plenty
// at the camera distances we render.
class WaterPlane {
public:
    WaterPlane() = default;
    ~WaterPlane();

    WaterPlane(const WaterPlane&) = delete;
    WaterPlane& operator=(const WaterPlane&) = delete;

    // size_blocks: total side length (centered on origin); kept ~slightly
    // larger than the visible chunk radius so the water sheet always
    // covers the horizon.
    // segments: number of quad subdivisions per side.
    bool init(float size_blocks, int segments);

    // Bind the VAO and issue the indexed draw. Caller is responsible for
    // having a water shader bound and the desired blend / depth state set.
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
