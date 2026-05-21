#pragma once

#include <glad/gl.h>

namespace gfx {

// Unit cube spanning [0, 1]^3 as 12 line segments (GL_LINES).
// Position-only VBO, owned RAII. Caller binds line width / shader.
class WireframeCube {
public:
    WireframeCube() = default;
    ~WireframeCube();
    WireframeCube(const WireframeCube&) = delete;
    WireframeCube& operator=(const WireframeCube&) = delete;

    bool init();
    void draw() const;

private:
    void destroy();
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
};

}  // namespace gfx
