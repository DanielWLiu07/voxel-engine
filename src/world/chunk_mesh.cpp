#include "world/chunk_mesh.h"

#include <chrono>
#include <glm/glm.hpp>

namespace world {

namespace {

// Solid-color "albedo" via UVs: we don't have a texture atlas yet, so
// pack a per-block tint into the UV channel and let the shader use it
// directly. The atlas swap-in lands with the greedy mesher commit.
//
// For now every face uses a 0..1 UV square so the existing checker
// texture tiles per-face — good enough to see the geometry.
constexpr float kFaceUV[4][2] = {
    {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
};

struct FaceDef {
    glm::vec3 normal;
    // Four corners of the quad relative to the block-min corner, in CCW
    // order when viewed from the outside (so back-face culling works).
    glm::vec3 corners[4];
};

// Order: +X, -X, +Y, -Y, +Z, -Z. Matches the neighbor offsets below.
constexpr FaceDef kFaces[6] = {
    // +X
    {{ 1,  0,  0}, {{1,0,0},{1,0,1},{1,1,1},{1,1,0}}},
    // -X
    {{-1,  0,  0}, {{0,0,1},{0,0,0},{0,1,0},{0,1,1}}},
    // +Y (top)
    {{ 0,  1,  0}, {{0,1,0},{1,1,0},{1,1,1},{0,1,1}}},
    // -Y (bottom)
    {{ 0, -1,  0}, {{0,0,1},{1,0,1},{1,0,0},{0,0,0}}},
    // +Z
    {{ 0,  0,  1}, {{1,0,1},{0,0,1},{0,1,1},{1,1,1}}},
    // -Z
    {{ 0,  0, -1}, {{0,0,0},{1,0,0},{1,1,0},{0,1,0}}},
};

constexpr int kNeighborOffsets[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

void emit_quad(ChunkMeshData& out, const glm::vec3& origin, const FaceDef& f) {
    std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());
    for (int i = 0; i < 4; ++i) {
        gfx::VertexPNT v;
        v.position = origin + f.corners[i];
        v.normal   = f.normal;
        v.uv       = {kFaceUV[i][0], kFaceUV[i][1]};
        out.vertices.push_back(v);
    }
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 3);
    ++out.quad_count;
}

}  // namespace

ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    ChunkMeshData out;
    // Rough preallocation: assume 2 quads per solid block on average,
    // which is roughly right for terrain (top + one side visible).
    out.vertices.reserve(static_cast<size_t>(chunk.solid_count()) * 8);
    out.indices.reserve(static_cast<size_t>(chunk.solid_count()) * 12);

    for (int y = 0; y < kChunkSizeY; ++y) {
        for (int z = 0; z < kChunkSizeZ; ++z) {
            for (int x = 0; x < kChunkSizeX; ++x) {
                BlockId self = chunk.get(x, y, z);
                if (!is_solid(self)) continue;

                glm::vec3 origin(static_cast<float>(x),
                                 static_cast<float>(y),
                                 static_cast<float>(z));

                for (int f = 0; f < 6; ++f) {
                    int nx = x + kNeighborOffsets[f][0];
                    int ny = y + kNeighborOffsets[f][1];
                    int nz = z + kNeighborOffsets[f][2];
                    BlockId neighbor = chunk.get_or_air(nx, ny, nz);
                    if (face_visible(self, neighbor)) {
                        emit_quad(out, origin, kFaces[f]);
                    }
                }
            }
        }
    }

    auto t1 = clock::now();
    out.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return out;
}

}  // namespace world
