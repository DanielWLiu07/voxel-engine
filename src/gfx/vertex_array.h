#pragma once

#include <glad/gl.h>

namespace gfx {

// A vertex array object with no buffers attached.
//
// Several passes here draw without vertex data at all - the sky's
// fullscreen triangle, the crosshair, and everything in the atmosphere,
// which derives each vertex from gl_VertexID. GL still requires a bound
// VAO for a draw call in the core profile, so these exist purely to
// satisfy that.
//
// They were raw GLuints in main with matching glDelete calls at the
// bottom of a three-thousand-line function, which is a pairing that
// holds right up until it does not: the cube added for the creatures was
// generated there and never deleted. Making the handle own itself is the
// fix that does not depend on anyone remembering.
class VertexArray {
public:
    VertexArray() = default;
    ~VertexArray() { destroy(); }
    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;
    VertexArray(VertexArray&& other) noexcept : id_(other.id_) { other.id_ = 0; }
    VertexArray& operator=(VertexArray&& other) noexcept {
        if (this != &other) {
            destroy();
            id_ = other.id_;
            other.id_ = 0;
        }
        return *this;
    }

    bool init() {
        destroy();
        glGenVertexArrays(1, &id_);
        return id_ != 0;
    }

    GLuint id() const { return id_; }

private:
    void destroy() {
        if (id_) glDeleteVertexArrays(1, &id_);
        id_ = 0;
    }
    GLuint id_ = 0;
};

}  // namespace gfx
