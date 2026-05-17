#include "world/chunk_mesh.h"

#include <chrono>
#include <cstring>
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

// ---------------------------------------------------------------------------
// Greedy mesher
// ---------------------------------------------------------------------------
//
// For each of 3 axes (x, y, z) and each of 2 directions (positive,
// negative), we sweep slices perpendicular to that axis. On each slice
// we build a 2D mask of "what face goes here" — populated only where
// the block on the "negative" side of the slice is solid AND the block
// on the "positive" side is not (or vice versa, for the opposite
// direction). We then walk the mask and merge contiguous same-id cells
// into the largest axis-aligned rectangle we can, emitting one quad
// per rectangle.
//
// Coordinate naming inside this function follows the canonical greedy-
// mesh paper: `d` is the sweep axis, `u` and `v` are the in-slice axes.

namespace {

// Map from (axis d, in-slice u, in-slice v, slice index) back to (x,y,z)
// world-space integer coords.
constexpr void slice_coords(int d, int u_axis, int v_axis,
                            int slice, int u, int v,
                            int& out_x, int& out_y, int& out_z) {
    int coords[3] = {0, 0, 0};
    coords[d]      = slice;
    coords[u_axis] = u;
    coords[v_axis] = v;
    out_x = coords[0];
    out_y = coords[1];
    out_z = coords[2];
}

// Axis sizes indexed by axis (0=x, 1=y, 2=z).
constexpr int axis_size(int axis) {
    return axis == 1 ? kChunkSizeY : kChunkSizeX;  // x and z are both 16
}

}  // namespace

ChunkMeshData build_chunk_mesh_greedy(const Chunk& chunk) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    ChunkMeshData out;
    out.vertices.reserve(static_cast<size_t>(chunk.solid_count()));
    out.indices.reserve(static_cast<size_t>(chunk.solid_count()) * 2);

    // For each axis d in {0,1,2}:
    //   u_axis, v_axis = the other two axes (we pick (d+1)%3 and (d+2)%3).
    //   For each direction (back = -1, front = +1):
    //     For each slice in [0, size(d)]:
    //       Build mask[u_size * v_size] of BlockId values where a face
    //       in that direction is visible, 0 (Air) elsewhere.
    //       Walk the mask, merging contiguous same-id cells into
    //       maximal-area rectangles, emit one quad each.

    for (int d = 0; d < 3; ++d) {
        const int u_axis = (d + 1) % 3;
        const int v_axis = (d + 2) % 3;
        const int d_size = axis_size(d);
        const int u_size = axis_size(u_axis);
        const int v_size = axis_size(v_axis);

        // Buffer reused per slice. Mask cells store BlockId; 0 == Air == "no face".
        std::vector<std::uint8_t> mask(static_cast<size_t>(u_size * v_size), 0);

        for (int dir : {-1, +1}) {
            // Slice index `s` lives between block s-1 and block s along axis d.
            // We iterate s from 0..d_size inclusive of the chunk boundary so
            // both faces of the outermost blocks are emitted.
            for (int s = 0; s <= d_size; ++s) {
                std::memset(mask.data(), 0, mask.size());

                // Populate the mask. For each (u, v) cell, check the two
                // blocks straddling slice s along axis d. A face is visible
                // if the block on the "owner" side is solid and the block
                // on the other side is not, where ownership depends on dir.
                for (int v = 0; v < v_size; ++v) {
                    for (int u = 0; u < u_size; ++u) {
                        int xa, ya, za, xb, yb, zb;
                        slice_coords(d, u_axis, v_axis, s - 1, u, v, xa, ya, za);
                        slice_coords(d, u_axis, v_axis, s,     u, v, xb, yb, zb);

                        BlockId a = chunk.get_or_air(xa, ya, za);
                        BlockId b = chunk.get_or_air(xb, yb, zb);

                        BlockId face = BlockId::Air;
                        if (dir == +1 && face_visible(a, b)) face = a;
                        if (dir == -1 && face_visible(b, a)) face = b;

                        mask[static_cast<size_t>(v * u_size + u)] =
                            static_cast<std::uint8_t>(face);
                    }
                }

                // Walk the mask and greedily merge.
                for (int v = 0; v < v_size; ++v) {
                    for (int u = 0; u < u_size; ) {
                        std::uint8_t id = mask[static_cast<size_t>(v * u_size + u)];
                        if (id == 0) { ++u; continue; }

                        // Extend width: how many consecutive cells in this row
                        // share the same block id?
                        int w = 1;
                        while (u + w < u_size
                               && mask[static_cast<size_t>(v * u_size + u + w)] == id) {
                            ++w;
                        }

                        // Extend height: how many rows below also match for
                        // the full width [u, u+w)?
                        int h = 1;
                        while (v + h < v_size) {
                            bool row_matches = true;
                            for (int k = 0; k < w; ++k) {
                                if (mask[static_cast<size_t>((v + h) * u_size + u + k)] != id) {
                                    row_matches = false;
                                    break;
                                }
                            }
                            if (!row_matches) break;
                            ++h;
                        }

                        // Emit one quad covering [u, u+w) x [v, v+h) on this slice.
                        glm::vec3 normal(0.0f);
                        normal[d] = static_cast<float>(dir);

                        // Build the four corners in world space.
                        // Corner P00 is at slice plane (s along d, u along
                        // u_axis, v along v_axis). The quad spans +w along
                        // u_axis and +h along v_axis.
                        auto make_corner = [&](int du, int dv) {
                            glm::vec3 c(0.0f);
                            c[d]      = static_cast<float>(s);
                            c[u_axis] = static_cast<float>(u + du);
                            c[v_axis] = static_cast<float>(v + dv);
                            return c;
                        };

                        glm::vec3 p00 = make_corner(0, 0);
                        glm::vec3 p10 = make_corner(w, 0);
                        glm::vec3 p11 = make_corner(w, h);
                        glm::vec3 p01 = make_corner(0, h);

                        std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());

                        // Winding depends on dir: we want CCW when viewed
                        // from the outside of the face for back-face culling.
                        auto push = [&](const glm::vec3& p, float uu, float vv) {
                            gfx::VertexPNT vtx;
                            vtx.position = p;
                            vtx.normal   = normal;
                            vtx.uv       = {uu, vv};
                            out.vertices.push_back(vtx);
                        };

                        if (dir > 0) {
                            push(p00, 0.0f, 0.0f);
                            push(p10, static_cast<float>(w), 0.0f);
                            push(p11, static_cast<float>(w), static_cast<float>(h));
                            push(p01, 0.0f, static_cast<float>(h));
                        } else {
                            push(p00, 0.0f, 0.0f);
                            push(p01, 0.0f, static_cast<float>(h));
                            push(p11, static_cast<float>(w), static_cast<float>(h));
                            push(p10, static_cast<float>(w), 0.0f);
                        }

                        out.indices.push_back(base + 0);
                        out.indices.push_back(base + 1);
                        out.indices.push_back(base + 2);
                        out.indices.push_back(base + 0);
                        out.indices.push_back(base + 2);
                        out.indices.push_back(base + 3);
                        ++out.quad_count;

                        // Zero out the merged region so we don't revisit.
                        for (int dv = 0; dv < h; ++dv) {
                            for (int du = 0; du < w; ++du) {
                                mask[static_cast<size_t>((v + dv) * u_size + u + du)] = 0;
                            }
                        }

                        u += w;
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
