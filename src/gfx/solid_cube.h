#pragma once

#include <glad/gl.h>

namespace gfx {

// Unit cube centred on the origin, position and normal, as 36 triangle
// vertices. The shaded counterpart to WireframeCube, and owned the same
// way: the destructor releases the GL names, and copying is deleted
// because two owners of one buffer means one of them deletes it twice.
//
// One buffer serves every creature. A creature is a couple of boxes
// scaled and placed by uniform, so nothing about it is per-instance
// vertex data - which is why this is a shared cube and not a mesh per
// entity.
class SolidCube {
public:
    SolidCube() = default;
    ~SolidCube();
    SolidCube(const SolidCube&) = delete;
    SolidCube& operator=(const SolidCube&) = delete;

    // Builds the buffer. Returns false if GL refused a name.
    bool init();
    // Draws the 36 vertices. Binds its own VAO; the caller owns the
    // shader and every uniform on it.
    void draw() const;

private:
    void destroy();
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
};

}  // namespace gfx
