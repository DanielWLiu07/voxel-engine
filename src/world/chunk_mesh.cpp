#include "world/chunk_mesh.h"

#include "core/profiler.h"

#include <chrono>
#include <cstring>
#include <glm/glm.hpp>

namespace world {

namespace {

constexpr float kFaceUV[4][2] = {
    {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
};

struct FaceDef {
    glm::vec3 normal;
    glm::vec3 corners[4];  // CCW from outside
};

// +X, -X, +Y, -Y, +Z, -Z
constexpr FaceDef kFaces[6] = {
    {{ 1, 0, 0}, {{1,0,0},{1,0,1},{1,1,1},{1,1,0}}},
    {{-1, 0, 0}, {{0,0,1},{0,0,0},{0,1,0},{0,1,1}}},
    {{ 0, 1, 0}, {{0,1,0},{1,1,0},{1,1,1},{0,1,1}}},
    {{ 0,-1, 0}, {{0,0,1},{1,0,1},{1,0,0},{0,0,0}}},
    {{ 0, 0, 1}, {{1,0,1},{0,0,1},{0,1,1},{1,1,1}}},
    {{ 0, 0,-1}, {{0,0,0},{1,0,0},{1,1,0},{0,1,0}}},
};

constexpr int kNeighborOffsets[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

void emit_quad(ChunkMeshData& out, const glm::vec3& origin, const FaceDef& f, BlockId id) {
    std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());
    for (int i = 0; i < 4; ++i) {
        gfx::VertexPNT v;
        v.position = origin + f.corners[i];
        v.normal   = f.normal;
        v.uv       = {kFaceUV[i][0], kFaceUV[i][1]};
        v.ao       = 1.0f;
        v.block_id = static_cast<float>(static_cast<int>(id));
        out.vertices.push_back(v);
    }
    out.indices.insert(out.indices.end(),
        {base+0, base+1, base+2, base+0, base+2, base+3});
    ++out.quad_count;
}

// 0fps voxel-AO formula. Returns 0..3 (3 = unoccluded).
inline int corner_ao(int side1, int side2, int corner) {
    if (side1 && side2) return 0;
    return 3 - (side1 + side2 + corner);
}

inline float ao_to_brightness(int ao) {
    constexpr float table[4] = {0.45f, 0.65f, 0.82f, 1.00f};
    return table[ao];
}

}  // namespace

ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk) {
    ZoneScopedN("mesh_naive");
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    ChunkMeshData out;
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
                    if (face_visible(self, chunk.get_or_air(nx, ny, nz))) {
                        emit_quad(out, origin, kFaces[f], self);
                    }
                }
            }
        }
    }

    out.build_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    return out;
}

namespace {

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

constexpr int axis_size(int axis) {
    return axis == 1 ? kChunkSizeY : kChunkSizeX;
}

}  // namespace

// Greedy mesh: sweep 6 (axis, dir) combos. Per slice, build a BlockId mask
// then merge contiguous same-id cells into maximal rectangles.
ChunkMeshData build_chunk_mesh_greedy(const Chunk& chunk) {
    ZoneScopedN("mesh_greedy");
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();

    ChunkMeshData out;
    if (chunk.empty()) {
        out.build_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        return out;
    }
    out.vertices.reserve(static_cast<size_t>(chunk.solid_count()));
    out.indices.reserve(static_cast<size_t>(chunk.solid_count()) * 2);

    for (int d = 0; d < 3; ++d) {
        const int u_axis = (d + 1) % 3;
        const int v_axis = (d + 2) % 3;
        const int d_size = axis_size(d);
        const int u_size = axis_size(u_axis);
        const int v_size = axis_size(v_axis);

        thread_local std::vector<std::uint8_t> mask;
        mask.assign(static_cast<size_t>(u_size * v_size), 0);

        for (int dir : {-1, +1}) {
            for (int s = 0; s <= d_size; ++s) {
                std::memset(mask.data(), 0, mask.size());

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

                        mask[static_cast<size_t>(v * u_size + u)] = static_cast<std::uint8_t>(face);
                    }
                }

                for (int v = 0; v < v_size; ++v) {
                    for (int u = 0; u < u_size; ) {
                        std::uint8_t id = mask[static_cast<size_t>(v * u_size + u)];
                        if (id == 0) { ++u; continue; }

                        int w = 1;
                        while (u + w < u_size
                               && mask[static_cast<size_t>(v * u_size + u + w)] == id) ++w;

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

                        glm::vec3 normal(0.0f);
                        normal[d] = static_cast<float>(dir);

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

                        // AO samples sit on the slice's outside cell.
                        const int out_d = (dir > 0) ? s : (s - 1);
                        auto sample_solid = [&](int sd, int su, int sv) -> int {
                            int xa, ya, za;
                            slice_coords(d, u_axis, v_axis, sd, su, sv, xa, ya, za);
                            return is_solid(chunk.get_or_air(xa, ya, za)) ? 1 : 0;
                        };
                        auto vertex_ao = [&](int du, int dv) {
                            const int U = u + du * w;
                            const int V = v + dv * h;
                            const int du_step = (du == 0) ? -1 : 0;
                            const int dv_step = (dv == 0) ? -1 : 0;
                            int s1 = sample_solid(out_d, U + du_step,             V + (dv == 0 ?  0 : -1));
                            int s2 = sample_solid(out_d, U + (du == 0 ? 0 : -1),  V + dv_step);
                            int sc = sample_solid(out_d, U + du_step,             V + dv_step);
                            return corner_ao(s1, s2, sc);
                        };

                        int ao00 = vertex_ao(0, 0);
                        int ao10 = vertex_ao(1, 0);
                        int ao11 = vertex_ao(1, 1);
                        int ao01 = vertex_ao(0, 1);

                        std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());
                        const float bf = static_cast<float>(id);
                        auto push = [&](const glm::vec3& p, float uu, float vv, int ao) {
                            gfx::VertexPNT vtx;
                            vtx.position = p;
                            vtx.normal   = normal;
                            vtx.uv       = {uu, vv};
                            vtx.ao       = ao_to_brightness(ao);
                            vtx.block_id = bf;
                            out.vertices.push_back(vtx);
                        };

                        if (dir > 0) {
                            push(p00, 0.0f, 0.0f, ao00);
                            push(p10, static_cast<float>(w), 0.0f, ao10);
                            push(p11, static_cast<float>(w), static_cast<float>(h), ao11);
                            push(p01, 0.0f, static_cast<float>(h), ao01);
                        } else {
                            push(p00, 0.0f, 0.0f, ao00);
                            push(p01, 0.0f, static_cast<float>(h), ao01);
                            push(p11, static_cast<float>(w), static_cast<float>(h), ao11);
                            push(p10, static_cast<float>(w), 0.0f, ao10);
                        }

                        // ao_flip: swap diagonals when one is more contrasty
                        // so the gradient doesn't appear torn.
                        bool flip = (ao00 + ao11) < (ao10 + ao01);
                        if (flip) {
                            out.indices.insert(out.indices.end(),
                                {base+1, base+2, base+3, base+1, base+3, base+0});
                        } else {
                            out.indices.insert(out.indices.end(),
                                {base+0, base+1, base+2, base+0, base+2, base+3});
                        }
                        ++out.quad_count;

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

    out.build_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    return out;
}

}  // namespace world
