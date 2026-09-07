#include "world/prism_mesh.h"

#include "core/profiler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace world {

namespace {

// A lattice cell key. Three ints, hashed the same way the chunk map hashes
// two - cells are dense in a small box so the mixing matters more than the
// spread.
struct CellKey {
    std::int32_t i, k, l;
    bool operator==(const CellKey& o) const {
        return i == o.i && k == o.k && l == o.l;
    }
};

struct CellKeyHash {
    std::size_t operator()(const CellKey& c) const noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(c.i) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint32_t>(c.k) * 0xBF58476D1CE4E5B9ull;
        h ^= static_cast<std::uint32_t>(c.l) * 0x94D049BB133111EBull;
        h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
        return static_cast<std::size_t>(h);
    }
};

// Which cell a slice point falls into, by evaluating the three linear
// forms and flooring. The inverse of cell_polygon, and the reason the
// mesher never has to search.
inline CellKey cell_at(const SliceBasis& b, float sx, float sz) {
    return {static_cast<std::int32_t>(std::floor(b.x4_sx * sx + b.x4_sz * sz + b.x4_c)),
            static_cast<std::int32_t>(std::floor(b.z4_sx * sx + b.z4_sz * sz + b.z4_c)),
            static_cast<std::int32_t>(std::floor(b.w4_sx * sx + b.w4_sz * sz + b.w4_c))};
}

inline bool solid(std::uint8_t b) { return is_solid(static_cast<BlockId>(b)); }

}  // namespace

PrismChunk build_prism_chunk(const TerrainGen4D& gen, ChunkCoord coord,
                             TerrainGen4D::Slice s) {
    ZoneScopedN("build_prism_chunk");
    PrismChunk pc;
    pc.coord = coord;
    pc.slice = s;

    const SliceBasis basis = SliceBasis::from(s);
    const float x0 = static_cast<float>(coord.x * kChunkSizeX);
    const float z0 = static_cast<float>(coord.z * kChunkSizeZ);
    constexpr float kSpan = static_cast<float>(kChunkSizeX);

    // A cell whose polygon is thinner than the vertex quantisation cannot
    // put a pixel on the screen: both of its walls round to the same
    // place. Dropping it here rather than at emit time keeps it out of
    // the neighbour map too, so the cells either side of it become each
    // other's neighbours and the surface stays closed.
    constexpr float kMinArea = 1e-4f;

    std::unordered_map<CellKey, std::int32_t, CellKeyHash> index;
    for_each_cell(basis, x0, z0, kSpan,
                  [&](int i, int k, int l, const SlicePolygon& p) {
        ++pc.candidates;
        if (std::fabs(polygon_area(p)) < kMinArea) return;
        PrismCell cell;
        cell.i = i; cell.k = k; cell.l = l;
        cell.poly = p;
        // Chunk-local, so the packed vertex's [0, 16] range applies.
        for (int v = 0; v < p.count; ++v) {
            cell.poly.x[v] = p.x[v] - x0;
            cell.poly.z[v] = p.z[v] - z0;
        }
        cell.across.fill(-1);
        index.emplace(CellKey{i, k, l},
                      static_cast<std::int32_t>(pc.cells.size()));
        pc.cells.push_back(cell);
    });

    // The column each cell holds. Sampled at the cell's centre in 4D, so
    // it does not depend on where the slice happens to cut through it.
    TerrainGen4D::Column4D col;
    for (auto& cell : pc.cells) {
        gen.fill_cell_column(cell.i, cell.k, cell.l, col);
        cell.blocks = col.blocks;
    }

    // Who is across each edge.
    //
    // Rather than tagging edges as the clipper produces them, step a
    // short way past the edge's midpoint along its outward normal and ask
    // which cell that lands in. It costs three dot products, it cannot
    // disagree with the tiling (it asks the same forms the tiling was
    // built from), and it gets the chunk boundary right for free: a step
    // that leaves the footprint finds no entry and stays -1.
    constexpr float kStep = 1e-3f;
    for (auto& cell : pc.cells) {
        const int n = cell.poly.count;
        for (int e = 0; e < n; ++e) {
            const int f = (e + 1) % n;
            const float ax = cell.poly.x[e], az = cell.poly.z[e];
            const float bx = cell.poly.x[f], bz = cell.poly.z[f];
            const float dx = bx - ax, dz = bz - az;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-6f) continue;
            // Outward normal of a positively wound polygon.
            const float nx =  dz / len, nz = -dx / len;
            const float mx = (ax + bx) * 0.5f + nx * kStep + x0;
            const float mz = (az + bz) * 0.5f + nz * kStep + z0;
            if (mx < x0 || mx > x0 + kSpan || mz < z0 || mz > z0 + kSpan) continue;
            const auto it = index.find(cell_at(basis, mx, mz));
            if (it != index.end()) cell.across[e] = it->second;
        }
    }
    return pc;
}

void rasterize_to_chunk(const PrismChunk& pc, Chunk& out) {
    ZoneScopedN("rasterize_prisms");
    const SliceBasis basis = SliceBasis::from(pc.slice);
    const float x0 = static_cast<float>(pc.coord.x * kChunkSizeX);
    const float z0 = static_cast<float>(pc.coord.z * kChunkSizeZ);

    std::unordered_map<CellKey, const PrismCell*, CellKeyHash> index;
    index.reserve(pc.cells.size() * 2);
    for (const auto& c : pc.cells) index.emplace(CellKey{c.i, c.k, c.l}, &c);

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            const auto it = index.find(cell_at(basis,
                                               x0 + static_cast<float>(x) + 0.5f,
                                               z0 + static_cast<float>(z) + 0.5f));
            if (it == index.end()) continue;
            const PrismCell& c = *it->second;
            for (int y = 0; y < kChunkSizeY; ++y) {
                out.set(x, y, z,
                        static_cast<BlockId>(c.blocks[static_cast<std::size_t>(y)]));
            }
        }
    }
}

namespace {

// A convex polygon as quads, so prism faces ride the engine-wide shared
// quad index buffer instead of needing an index buffer of their own.
//
// Fan pairs: (v0,v1,v2,v3), (v0,v3,v4,v5), and so on. An odd polygon ends
// on a quad whose last two vertices coincide, which the fixed
// {0,1,2},{0,2,3} pattern turns into one real triangle and one of zero
// area. A degenerate triangle rasterises no pixels, so a five-sided cap
// costs the same two quads a six-sided one does and nothing else.
template <typename Emit>
void fan_quads(int n, Emit&& emit) {
    for (int j = 1; j + 1 < n; j += 2) {
        const int c = (j + 2 <= n - 1) ? j + 2 : j + 1;
        emit(0, j, j + 1, c);
    }
}

struct PrismVertex {
    float x, y, z;
    float u, v;
};

void push_quad(ChunkMeshData& out, const PrismVertex q[4], std::uint8_t normal,
               std::uint8_t id, std::uint8_t light) {
    for (int i = 0; i < 4; ++i) {
        gfx::VertexPacked p;
        p.x = gfx::quantize_sub_unit_xz(q[i].x);
        p.z = gfx::quantize_sub_unit_xz(q[i].z);
        p.y = static_cast<std::uint16_t>(q[i].y);
        p.normal = normal;
        p.ao = 3;
        p.u = gfx::quantize_sub_unit_uv(q[i].u);
        p.v = gfx::quantize_sub_unit_uv(q[i].v);
        p.block_id = id;
        p.light = light;
        out.vertices.push_back(p);
    }
    ++out.quad_count;
}

}  // namespace

ChunkMeshData build_prism_mesh(const PrismChunk& pc, const LightSource& light) {
    ZoneScopedN("build_prism_mesh");
    const auto t0 = std::chrono::steady_clock::now();

    ChunkMeshData out;
    out.xz_scale = gfx::kSubUnitXZScale;
    out.uv_scale = gfx::kSubUnitUVScale;
    (void)light;  // block light arrives in its own change

    // Normal indices 2 and 3 are +Y and -Y in the packed table.
    constexpr std::uint8_t kUp = 2, kDown = 3;

    PrismVertex q[4];
    for (const auto& cell : pc.cells) {
        const int n = cell.poly.count;
        if (n < 3) continue;

        int y = 0;
        while (y < kChunkSizeY) {
            const std::uint8_t id = cell.blocks[static_cast<std::size_t>(y)];
            if (!solid(id)) { ++y; continue; }
            int y1 = y;
            while (y1 + 1 < kChunkSizeY &&
                   cell.blocks[static_cast<std::size_t>(y1 + 1)] == id) ++y1;

            // Caps. The polygon is positively wound, which is the -Y
            // winding this engine uses; +Y wants it reversed.
            const bool cap_top = (y1 + 1 >= kChunkSizeY) ||
                !solid(cell.blocks[static_cast<std::size_t>(y1 + 1)]);
            const bool cap_bottom = (y == 0) ||
                !solid(cell.blocks[static_cast<std::size_t>(y - 1)]);

            if (cap_top) {
                const float top_y = static_cast<float>(y1 + 1);
                fan_quads(n, [&](int a, int b, int c, int d) {
                    const int src[4] = {n - 1 - a, n - 1 - b, n - 1 - c, n - 1 - d};
                    for (int v = 0; v < 4; ++v) {
                        q[v] = {cell.poly.x[src[v]], top_y, cell.poly.z[src[v]],
                                cell.poly.x[src[v]], cell.poly.z[src[v]]};
                    }
                    push_quad(out, q, kUp, id, kMaxLight);
                });
            }
            if (cap_bottom) {
                const float bot_y = static_cast<float>(y);
                fan_quads(n, [&](int a, int b, int c, int d) {
                    const int src[4] = {a, b, c, d};
                    for (int v = 0; v < 4; ++v) {
                        q[v] = {cell.poly.x[src[v]], bot_y, cell.poly.z[src[v]],
                                cell.poly.x[src[v]], cell.poly.z[src[v]]};
                    }
                    push_quad(out, q, kDown, id, kMaxLight);
                });
            }

            // Walls. One per polygon edge, cut into vertical runs by
            // whether the cell across that edge is solid at each y - a
            // wall against a neighbour's rock is not drawn, and a wall
            // against its cave mouth is.
            for (int e = 0; e < n; ++e) {
                const int f = (e + 1) % n;
                const float ax = cell.poly.x[e], az = cell.poly.z[e];
                const float bx = cell.poly.x[f], bz = cell.poly.z[f];
                const float dx = bx - ax, dz = bz - az;
                const float len = std::sqrt(dx * dx + dz * dz);
                if (len < 1e-4f) continue;
                const std::uint8_t nrm = gfx::encode_horizontal_normal(dz, -dx);
                const PrismCell* other = (cell.across[e] >= 0)
                    ? &pc.cells[static_cast<std::size_t>(cell.across[e])]
                    : nullptr;

                int wy = y;
                while (wy <= y1) {
                    auto hidden = [&](int yy) {
                        return other != nullptr &&
                               solid(other->blocks[static_cast<std::size_t>(yy)]);
                    };
                    if (hidden(wy)) { ++wy; continue; }
                    int wy1 = wy;
                    while (wy1 + 1 <= y1 && !hidden(wy1 + 1)) ++wy1;
                    const float lo = static_cast<float>(wy);
                    const float hi = static_cast<float>(wy1 + 1);
                    q[0] = {ax, lo, az, 0.0f,  lo};
                    q[1] = {ax, hi, az, 0.0f,  hi};
                    q[2] = {bx, hi, bz, len,   hi};
                    q[3] = {bx, lo, bz, len,   lo};
                    push_quad(out, q, nrm, id, kMaxLight);
                    wy = wy1 + 1;
                }
            }

            y = y1 + 1;
        }
    }

    out.build_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace world
