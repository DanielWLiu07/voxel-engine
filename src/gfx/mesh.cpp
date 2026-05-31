#include "gfx/mesh.h"

namespace gfx {

Mesh::~Mesh() { destroy(); }

Mesh::Mesh(Mesh&& o) noexcept
    : vao_(o.vao_), vbo_(o.vbo_), ebo_(o.ebo_), index_count_(o.index_count_) {
    o.vao_ = o.vbo_ = o.ebo_ = 0;
    o.index_count_ = 0;
}

Mesh& Mesh::operator=(Mesh&& o) noexcept {
    if (this != &o) {
        destroy();
        vao_ = o.vao_; vbo_ = o.vbo_; ebo_ = o.ebo_;
        index_count_ = o.index_count_;
        o.vao_ = o.vbo_ = o.ebo_ = 0;
        o.index_count_ = 0;
    }
    return *this;
}

void Mesh::destroy() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    index_count_ = 0;
}

void Mesh::upload(std::span<const VertexPNT> vertices,
                  std::span<const std::uint32_t> indices) {
    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!vbo_) glGenBuffers(1, &vbo_);
    if (!ebo_) glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size_bytes()),
                 vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size_bytes()),
                 indices.data(), GL_STATIC_DRAW);

    auto attr = [](GLuint loc, GLint comps, std::size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, comps, GL_FLOAT, GL_FALSE,
                              sizeof(VertexPNT), reinterpret_cast<void*>(offset));
    };
    attr(0, 3, offsetof(VertexPNT, position));
    attr(1, 3, offsetof(VertexPNT, normal));
    attr(2, 2, offsetof(VertexPNT, uv));
    attr(3, 1, offsetof(VertexPNT, ao));
    attr(4, 1, offsetof(VertexPNT, block_id));

    glBindVertexArray(0);
    index_count_ = indices.size();
}

void Mesh::draw() const {
    if (!vao_ || index_count_ == 0) return;
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count_),
                   GL_UNSIGNED_INT, nullptr);
}

void Mesh::bind() const {
    if (vao_) glBindVertexArray(vao_);
}

void Mesh::draw_range_bound(std::size_t index_offset, std::size_t count) const {
    if (!vao_ || count == 0) return;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(index_offset * sizeof(std::uint32_t)));
}

}  // namespace gfx
