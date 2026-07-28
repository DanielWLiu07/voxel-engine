#include "gfx/mesh.h"

#include <algorithm>
#include <cassert>
#include <vector>

namespace gfx {

QuadIndexBuffer::~QuadIndexBuffer() {
    if (ebo_) glDeleteBuffers(1, &ebo_);
}

void QuadIndexBuffer::bind_for(std::size_t quad_count) {
    if (!ebo_) glGenBuffers(1, &ebo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    if (quad_count <= capacity_quads_) return;
    // Grow past the request so the next slightly-bigger chunk doesn't
    // rebuild the pattern again.
    const std::size_t new_cap =
        std::max(quad_count + quad_count / 2, std::size_t{1024});
    std::vector<std::uint32_t> pattern;
    pattern.reserve(new_cap * 6);
    for (std::size_t q = 0; q < new_cap; ++q) {
        const auto b = static_cast<std::uint32_t>(4 * q);
        pattern.insert(pattern.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(pattern.size() * sizeof(std::uint32_t)),
                 pattern.data(), GL_STATIC_DRAW);
    capacity_quads_ = new_cap;
}

Mesh::~Mesh() { destroy(); }

Mesh::Mesh(Mesh&& o) noexcept
    : vao_(o.vao_), vbo_(o.vbo_), shared_ebo_(o.shared_ebo_),
      index_count_(o.index_count_) {
    o.vao_ = o.vbo_ = o.shared_ebo_ = 0;
    o.index_count_ = 0;
}

Mesh& Mesh::operator=(Mesh&& o) noexcept {
    if (this != &o) {
        destroy();
        vao_ = o.vao_; vbo_ = o.vbo_; shared_ebo_ = o.shared_ebo_;
        index_count_ = o.index_count_;
        o.vao_ = o.vbo_ = o.shared_ebo_ = 0;
        o.index_count_ = 0;
    }
    return *this;
}

void Mesh::destroy() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    shared_ebo_ = 0;  // owned by the QuadIndexBuffer, never deleted here
    index_count_ = 0;
}

void Mesh::upload(std::span<const VertexPacked> vertices,
                  QuadIndexBuffer& quad_indices) {
    assert(vertices.size() % 4 == 0 && "quad meshes only");
    const std::size_t quad_count = vertices.size() / 4;
    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!vbo_) glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size_bytes()),
                 vertices.data(), GL_STATIC_DRAW);

    // The element-array binding is VAO state: this latches the shared quad
    // pattern into the VAO (growing it first if needed), replacing a
    // per-mesh index upload.
    quad_indices.bind_for(quad_count);
    shared_ebo_ = quad_indices.id();

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
    index_count_ = quad_count * 6;
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
    if (!vbo_ || !shared_ebo_) return;
    GLint vbytes = 0;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vbytes);
    vertices.resize(static_cast<std::size_t>(vbytes) / sizeof(VertexPacked));
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, vbytes, vertices.data());
    // The shared buffer is sized for the largest mesh; only the first
    // index_count_ entries are this mesh's draw range.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shared_ebo_);
    indices.resize(index_count_);
    glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
                       static_cast<GLsizeiptr>(index_count_ * sizeof(std::uint32_t)),
                       indices.data());
}

void Mesh::draw_range_bound(std::size_t index_offset, std::size_t count) const {
    if (!vao_ || count == 0) return;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT,
                   reinterpret_cast<void*>(index_offset * sizeof(std::uint32_t)));
}

}  // namespace gfx
