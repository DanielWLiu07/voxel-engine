#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gfx {

struct VertexPNT {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    float     ao;        // 0 occluded .. 1 unoccluded
    float     block_id;  // palette index
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    void upload(std::span<const VertexPNT> vertices, std::span<const std::uint32_t> indices);
    void draw() const;

    // Index-range draw for sliced meshes (e.g. per-section sub-chunks sharing
    // one VBO per chunk). Caller is responsible for binding the VAO first
    // (typically via bind() before the first draw_range_bound in a batch).
    void bind() const;
    void draw_range_bound(std::size_t index_offset, std::size_t index_count) const;

    std::size_t index_count() const { return index_count_; }

    // Debug-only: pulls the uploaded VBO/EBO back off the GPU so a validator
    // can check exactly what gets drawn (not what the CPU thinks it sent).
    void debug_read_back(std::vector<VertexPNT>& vertices,
                         std::vector<std::uint32_t>& indices) const;

private:
    void destroy();

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    std::size_t index_count_ = 0;
};

}  // namespace gfx
