#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gfx {

// The 6 axis directions a packed vertex's normal index selects, in
// +X,-X,+Y,-Y,+Z,-Z order. The vertex shader carries the same table.
inline constexpr glm::vec3 kPackedNormals[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// Chunk-mesh vertex, packed to 12 bytes (the float layout it replaced was
// 40). Every field is exactly representable: positions are mesh-local
// integers (x,z in [0,16], y in [0,256]), the normal is one of 6 axis
// directions, ao is the raw 0..3 voxel-AO level (the brightness table
// lives in the vertex shader), uv spans are integer run lengths. Uploaded
// with glVertexAttribIPointer as three integer attributes; the vertex
// shader decodes. Field order is the attribute layout - do not reorder.
struct VertexPacked {
    std::uint8_t  x = 0, z = 0;   // attribute 0: x, z, normal, ao (4 x u8)
    std::uint8_t  normal = 0;     // index into kPackedNormals
    std::uint8_t  ao = 3;         // 0 occluded .. 3 unoccluded
    std::uint16_t y = 0;          // attribute 1: y, u, v (3 x u16)
    std::uint16_t u = 0, v = 0;
    std::uint8_t  block_id = 0;   // attribute 2: block id (u8)
    std::uint8_t  pad = 0;

    glm::vec3 pos() const {
        return {static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(z)};
    }
    glm::vec3 nrm() const { return kPackedNormals[normal]; }
};
static_assert(sizeof(VertexPacked) == 12, "packed layout drifted");

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    void upload(std::span<const VertexPacked> vertices, std::span<const std::uint32_t> indices);
    void draw() const;

    // Index-range draw for sliced meshes (e.g. per-section sub-chunks sharing
    // one VBO per chunk). Caller is responsible for binding the VAO first
    // (typically via bind() before the first draw_range_bound in a batch).
    void bind() const;
    void draw_range_bound(std::size_t index_offset, std::size_t index_count) const;

    std::size_t index_count() const { return index_count_; }

    // Debug-only: pulls the uploaded VBO/EBO back off the GPU so a validator
    // can check exactly what gets drawn (not what the CPU thinks it sent).
    void debug_read_back(std::vector<VertexPacked>& vertices,
                         std::vector<std::uint32_t>& indices) const;

private:
    void destroy();

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    std::size_t index_count_ = 0;
};

}  // namespace gfx
