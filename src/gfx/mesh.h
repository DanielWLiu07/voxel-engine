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

// One element buffer shared by every quad mesh. All chunk meshes
// triangulate quad q the same way - {4q+0, 4q+1, 4q+2, 4q+0, 4q+2, 4q+3} -
// so per-mesh index buffers were pure redundancy (a third of the resident
// mesh bytes). The buffer holds that fixed pattern, grown to the largest
// mesh seen; every mesh's VAO binds it by name, and growing the data store
// in place keeps existing VAOs valid (VAOs reference buffers, not
// contents). The AO diagonal flip that used to vary the per-quad index
// pattern is encoded by rotating the quad's vertex order at emit time.
class QuadIndexBuffer {
public:
    QuadIndexBuffer() = default;
    ~QuadIndexBuffer();

    QuadIndexBuffer(const QuadIndexBuffer&) = delete;
    QuadIndexBuffer& operator=(const QuadIndexBuffer&) = delete;

    // Binds the buffer into the currently bound VAO, first growing the
    // pattern if quad_count exceeds capacity. Call with the target VAO
    // bound - the element-array binding is VAO state.
    void bind_for(std::size_t quad_count);

    GLuint id() const { return ebo_; }
    std::size_t bytes() const {
        return capacity_quads_ * 6 * sizeof(std::uint32_t);
    }

private:
    GLuint ebo_ = 0;
    std::size_t capacity_quads_ = 0;
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    // vertices.size() must be a multiple of 4 (whole quads); triangulation
    // comes from the shared quad pattern, which outlives this mesh.
    void upload(std::span<const VertexPacked> vertices,
                QuadIndexBuffer& quad_indices);
    void draw() const;

    // Index-range draw for sliced meshes (e.g. per-section sub-chunks sharing
    // one VBO per chunk). Caller is responsible for binding the VAO first
    // (typically via bind() before the first draw_range_bound in a batch).
    void bind() const;
    void draw_range_bound(std::size_t index_offset, std::size_t index_count) const;

    std::size_t index_count() const { return index_count_; }

    // Debug-only: pulls this mesh's vertices and the index range it draws
    // back off the GPU (the indices come from the shared quad buffer) so a
    // validator can check exactly what gets drawn, not what the CPU thinks
    // it sent.
    void debug_read_back(std::vector<VertexPacked>& vertices,
                         std::vector<std::uint32_t>& indices) const;

private:
    void destroy();

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint shared_ebo_ = 0;  // non-owning; the QuadIndexBuffer deletes it
    std::size_t index_count_ = 0;
};

}  // namespace gfx
