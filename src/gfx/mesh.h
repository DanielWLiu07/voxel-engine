#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gfx {

// The 6 axis directions a packed vertex's normal index selects, in
// +X,-X,+Y,-Y,+Z,-Z order. The vertex shader carries the same table.
inline constexpr glm::vec3 kPackedNormals[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// Indices 6..255 are horizontal directions, 250 of them evenly spaced.
//
// A voxel face points along an axis and six values cover it. A 4D block's
// cross-section does not: turn the cut in both planes and a cell presents
// a hexagonal pillar whose walls face whichever way the clip left them.
// Those walls are always HORIZONTAL - y is the one axis the slice
// rotation leaves alone, so a prism's sides are vertical and its caps are
// flat - which is why a single angle describes them and a full normal
// encoding is not needed.
//
// 250 directions is 1.44 degrees apart, so a wall's shading is off by at
// most 0.72 degrees. Lambert over that error moves a face's brightness by
// under 0.01%. The alternative was a wider vertex, and the 12-byte stride
// is load-bearing for a published figure.
//
// The first six entries are untouched, so every vertex the cube mesher
// has ever emitted decodes exactly as before.
inline constexpr int kAxisNormalCount  = 6;
inline constexpr int kHorizNormalCount = 250;
static_assert(kAxisNormalCount + kHorizNormalCount <= 256,
              "the normal index is one byte");

inline constexpr float kTwoPi = 6.28318530717958647692f;

inline glm::vec3 decode_packed_normal(unsigned idx) {
    if (idx < static_cast<unsigned>(kAxisNormalCount))
        return kPackedNormals[idx];
    const float a = static_cast<float>(idx - kAxisNormalCount) *
                    (kTwoPi / static_cast<float>(kHorizNormalCount));
    return {std::cos(a), 0.0f, std::sin(a)};
}

// Nearest horizontal index for a direction in the XZ plane. The input
// need not be normalized; only its angle is read.
inline std::uint8_t encode_horizontal_normal(float nx, float nz) {
    float a = std::atan2(nz, nx);
    if (a < 0.0f) a += kTwoPi;
    int q = static_cast<int>(std::lround(
        a * (static_cast<float>(kHorizNormalCount) / kTwoPi)));
    q %= kHorizNormalCount;
    return static_cast<std::uint8_t>(kAxisNormalCount + q);
}

// Sub-block position quantisation, for meshes whose vertices do not land
// on the voxel lattice.
//
// The cube mesher's x and z are integers in [0, 16] and go into the two
// position bytes as themselves. A prism's corners are wherever the
// hyperplane cut them, so they need a fraction, and the byte pair has to
// carry [0, 16] in 255 steps rather than 16. The scale rides in u_model
// as a non-uniform (s, 1, s), which is exact for these meshes: prism
// normals are either straight up, straight down, or purely horizontal,
// and a diagonal scale leaves all three pointing where they were once
// the shader normalizes.
//
// 16/255 is 6.3 cm of quantisation. It cannot open a crack between two
// cells: neighbours share a vertex COORDINATE, and the same float rounds
// the same way, so a shared corner stays shared. A sliver thinner than
// one step collapses to nothing, which is the correct outcome for a
// face thinner than a twentieth of a block.
inline constexpr float kSubUnitXZScale = 16.0f / 255.0f;

// Texture coordinates in 1/64 of a block. The cube mesher emits whole-run
// lengths and uses a scale of 1; a prism needs fractions of a block along
// a wall, and 1/64 leaves room for a 256-block vertical run (16384) well
// inside the u16.
inline constexpr float kSubUnitUVScale = 1.0f / 64.0f;

inline std::uint8_t quantize_sub_unit_xz(float v) {
    const float q = v / kSubUnitXZScale;
    return static_cast<std::uint8_t>(
        std::lround(q < 0.0f ? 0.0f : (q > 255.0f ? 255.0f : q)));
}

inline std::uint16_t quantize_sub_unit_uv(float v) {
    const float q = v / kSubUnitUVScale;
    return static_cast<std::uint16_t>(
        std::lround(q < 0.0f ? 0.0f : (q > 65535.0f ? 65535.0f : q)));
}

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
    std::uint8_t  block_id = 0;   // attribute 2: block id, light (2 x u8)
    // Block light 0..15. This byte was padding: the struct needed 12 for
    // alignment whether it was used or not, so carrying a light level per
    // vertex costs nothing at all - the stride, its static_assert, and
    // every memory figure in the README are unchanged by it.
    std::uint8_t  light = 0;

    glm::vec3 pos() const {
        return {static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(z)};
    }
    glm::vec3 nrm() const { return decode_packed_normal(normal); }
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

    // Index-range draw for sliced meshes (e.g. per-section sub-chunks sharing
    // one VBO per chunk). Caller is responsible for binding the VAO first
    // (typically via bind() before the first draw_range_bound in a batch).
    void bind() const;
    void draw_range_bound(std::size_t index_offset, std::size_t index_count) const;

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
