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

void Mesh::upload(std::span<const VertexPacked> vertices,
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

    // Integer attributes (glVertexAttribIPointer, not the normalizing
    // float path); the vertex shader decodes positions, normal index, AO
    // level, and uv spans from raw unsigned integers.
    auto iattr = [](GLuint loc, GLint comps, GLenum type, std::size_t offset) {
        glEnableVertexAttribArray(loc);
        glVertexAttribIPointer(loc, comps, type, sizeof(VertexPacked),
                               reinterpret_cast<void*>(offset));
    };
    iattr(0, 4, GL_UNSIGNED_BYTE,  offsetof(VertexPacked, x));
    iattr(1, 3, GL_UNSIGNED_SHORT, offsetof(VertexPacked, y));
    iattr(2, 1, GL_UNSIGNED_BYTE,  offsetof(VertexPacked, block_id));

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

void Mesh::debug_read_back(std::vector<VertexPacked>& vertices,
                           std::vector<std::uint32_t>& indices) const {
    vertices.clear();
    indices.clear();
    if (!vbo_ || !ebo_) return;
    GLint vbytes = 0, ibytes = 0;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vbytes);
    vertices.resize(static_cast<std::size_t>(vbytes) / sizeof(VertexPacked));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, vbytes, vertices.data());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &ibytes);
    indices.resize(static_cast<std::size_t>(ibytes) / sizeof(std::uint32_t));
    glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, ibytes, indices.data());
}

void Mesh::draw_range_bound(std::size_t index_offset, std::size_t count) const {
    if (!vao_ || count == 0) return;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(index_offset * sizeof(std::uint32_t)));
}

}  // namespace gfx
