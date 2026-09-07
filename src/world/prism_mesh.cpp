#include "world/prism_mesh.h"

#include "core/profiler.h"

#include "gfx/mesh.h"

#include "world/terrain_gen.h"   // altitude band constants
#include "world/tree_stamps.h"

#include <algorithm>
#include <utility>
#include <cassert>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace world {

namespace {

// Snap one coordinate to the grid the packed vertex can address.
//
// Shared by the cell tiling and by merged boxes, and it has to be, or a
// merged cap's edge and the wall hanging off it would round to different
// places and open a crack along every merge boundary.
inline float snap_xz(float v) {
    return static_cast<float>(gfx::quantize_sub_unit_xz(v)) *
           gfx::kSubUnitXZScale;
}

// The polygon's centroid, as a voxel of the chunk. Used wherever a cell
// has to be matched to voxel storage - block light is flood-filled on the
// grid, not on the lattice. The centroid is interior to a convex polygon
// with area, so it lands in the cell it came from.
inline void centroid_voxel(const SlicePolygon& p, int& vx, int& vz) {
    float cx = 0.0f, cz = 0.0f;
    for (int v = 0; v < p.count; ++v) { cx += p.x[v]; cz += p.z[v]; }
    cx /= static_cast<float>(p.count);
    cz /= static_cast<float>(p.count);
    vx = std::clamp(static_cast<int>(cx), 0, kChunkSizeX - 1);
    vz = std::clamp(static_cast<int>(cz), 0, kChunkSizeZ - 1);
}

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

namespace {

// The tiling alone: which cells, what shape, who is next to whom. Both
// column sources share it, and neither can change it - a cell's shape is
// a fact about the cut, not about what is inside the cell.
PrismChunk tile_footprint(ChunkCoord coord, TerrainGen4D::Slice s) {
    ZoneScopedN("tile_footprint");
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
        {
            int vx = 0, vz = 0;
            centroid_voxel(cell.poly, vx, vz);
            cell.vx = static_cast<std::int16_t>(vx);
            cell.vz = static_cast<std::int16_t>(vz);
        }
        index.emplace(CellKey{i, k, l},
                      static_cast<std::int32_t>(pc.cells.size()));
        pc.cells.push_back(cell);
    });

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

    // Snap every corner to the grid the packed vertex can actually
    // address, and drop any edge that collapses.
    //
    // Done HERE, once, rather than at emit time, and that is the whole
    // point. A wall shorter than one quantisation step has its direction
    // rotated by rounding - measured at 78 degrees on a real chunk, from
    // an edge running nearly along +x whose two ends both landed on the
    // same x byte - so the triangle the GPU receives faces somewhere the
    // stored normal does not. With backface culling that is a hole, and
    // it is a hole nothing upstream can see, because every part of the
    // mesher was reasoning about a polygon the GPU never gets.
    //
    // Snapping first means the polygon in hand IS the polygon uploaded:
    // normals, winding, uv lengths and cap areas are all computed from
    // the same coordinates, so they cannot disagree. Sub-step edges do
    // not become badly-facing slivers, they cease to exist, which is the
    // correct answer for a face 6 cm wide.
    //
    // It also cannot open a crack: two cells name a shared corner with
    // the same float, and the same float snaps the same way, so a corner
    // that survives survives on both sides and one that collapses
    // collapses on both.
    //
    // The neighbour probe above runs on the exact polygon on purpose. It
    // steps a hair past an edge midpoint to ask which cell is over there,
    // and a snapped midpoint can be 3 cm off the real one - enough to
    // land in the wrong cell where the tiling is fine.
    for (auto& cell : pc.cells) {
        SlicePolygon snapped;
        // (see snap_polygon below - kept inline here because the loop also
        // has to carry the neighbour list across a corner merge)
        std::array<std::int32_t, SlicePolygon::kMaxVerts> across{};
        across.fill(-1);
        for (int v = 0; v < cell.poly.count; ++v) {
            const float sx = snap_xz(cell.poly.x[v]);
            const float sz = snap_xz(cell.poly.z[v]);
            if (snapped.count > 0 &&
                snapped.x[snapped.count - 1] == sx &&
                snapped.z[snapped.count - 1] == sz) {
                // This corner merged with the previous one. The edge
                // between them is gone; the edge LEAVING this corner is
                // the one that survives, so it takes the slot.
                across[snapped.count - 1] = cell.across[v];
                continue;
            }
            snapped.x[snapped.count] = sx;
            snapped.z[snapped.count] = sz;
            across[snapped.count] = cell.across[v];
            ++snapped.count;
        }
        // The wrap-around pair, which the loop above cannot see.
        while (snapped.count > 1 &&
               snapped.x[0] == snapped.x[snapped.count - 1] &&
               snapped.z[0] == snapped.z[snapped.count - 1]) {
            --snapped.count;
        }
        if (snapped.count < 3) snapped.count = 0;
        cell.poly = snapped;
        cell.across = across;
    }
    return pc;
}


}  // namespace

namespace {

using CellIndex = std::unordered_map<CellKey, std::int32_t, CellKeyHash>;

CellIndex index_of(PrismChunk& pc) {
    CellIndex ix;
    ix.reserve(pc.cells.size() * 2);
    for (std::size_t n = 0; n < pc.cells.size(); ++n) {
        ix.emplace(CellKey{pc.cells[n].i, pc.cells[n].k, pc.cells[n].l},
                   static_cast<std::int32_t>(n));
    }
    return ix;
}

// Where a tree writes, in the lattice.
//
// A tree is the one feature of this generator that spills sideways, and
// that makes it the one that has to know which space it is spilling in. A
// cube-path tree spreads across voxel columns; here it spreads across
// lattice cells at a fixed w, so it stays one 4D object - a tree occupies
// a slab of the fourth dimension exactly as a placed block does, and
// turning the cut cuts through it rather than deleting it.
//
// Out of bounds means "no such cell in this chunk's tiling", which is the
// chunk boundary. Stamps clip there exactly as the cube path's clip at
// in_chunk_bounds.
struct CellSink {
    std::vector<PrismCell>* cells;
    const CellIndex*        index;
    std::int32_t            l;

    std::int32_t find(int i, int k) const {
        const auto it = index->find(CellKey{i, k, l});
        return (it == index->end()) ? -1 : it->second;
    }
    bool in_bounds(int i, int y, int k) const {
        return y >= 0 && y < kChunkSizeY && find(i, k) >= 0;
    }
    BlockId get(int i, int y, int k) const {
        const std::int32_t n = find(i, k);
        if (n < 0) return BlockId::Air;
        return static_cast<BlockId>(
            (*cells)[static_cast<std::size_t>(n)]
                .blocks[static_cast<std::size_t>(y)]);
    }
    void set(int i, int y, int k, BlockId b) {
        const std::int32_t n = find(i, k);
        if (n < 0) return;
        (*cells)[static_cast<std::size_t>(n)]
            .blocks[static_cast<std::size_t>(y)] =
            static_cast<std::uint8_t>(b);
    }
};

// The largest density the tree rule can produce, so a cell can be
// rejected on one hash before its column is generated.
//
// The rule is 0.012 + max(0, biome) * 0.025 and biome is bounded by 1, so
// nothing above 0.037 can ever plant. That gate rejects about 96% of
// candidates for the cost of a multiply, which is what makes it
// affordable to consider every cell that could reach into this chunk
// rather than only the ones inside it.
constexpr float kMaxTreeDensity = 0.037f;

}  // namespace

PrismChunk build_prism_chunk(const TerrainGen4D& gen, ChunkCoord coord,
                             TerrainGen4D::Slice s) {
    ZoneScopedN("build_prism_chunk");
    PrismChunk pc = tile_footprint(coord, s);
    TerrainGen4D::Column4D col;
    for (auto& cell : pc.cells) {
        gen.fill_cell_column(cell.i, cell.k, cell.l, col);
        cell.blocks = col.blocks;
    }

    // Trees.
    //
    // Pushed from every cell that could REACH one of ours, not only from
    // the ones inside the chunk. A canopy is five cells across, so
    // planting only from cells the chunk owns would leave a tree-free
    // band around every chunk - the artifact the cube path has and the
    // reason its trees stop two blocks short of each boundary.
    //
    // The dilation is in the lattice and at a fixed w, which is the same
    // statement as "a tree belongs to one slab of the fourth dimension".
    ZoneScopedN("prism_trees");
    const CellIndex ix = index_of(pc);
    constexpr int kReach = 2;
    std::unordered_set<CellKey, CellKeyHash> sources;
    sources.reserve(pc.cells.size() * 4);
    for (const auto& cell : pc.cells) {
        for (int di = -kReach; di <= kReach; ++di)
            for (int dk = -kReach; dk <= kReach; ++dk)
                sources.insert(CellKey{cell.i + di, cell.k + dk, cell.l});
    }

    for (const CellKey& src : sources) {
        // Same draw the cube path makes, on the same two coordinates, so
        // an untilted cut plants the same trees in the same places.
        const float r = hash2d_f(src.i, src.k, 0x7B1E5A2D);
        if (r > kMaxTreeDensity) continue;

        gen.fill_cell_column(src.i, src.k, src.l, col);
        if (col.is_desert) continue;
        if (col.guide_height <= kSeaLevel + kSandBand) continue;
        if (col.guide_height >= kStoneBand) continue;
        const int h = col.top;
        if (h + 8 >= kChunkSizeY) continue;
        if (static_cast<BlockId>(col.blocks[static_cast<std::size_t>(h)])
            != BlockId::Grass) continue;
        const float density = 0.012f + std::max(0.0f, col.biome) * 0.025f;
        if (r > density) continue;

        CellSink sink{&pc.cells, &ix, src.l};
        const float pick = hash2d_f(src.i + 17, src.k + 41, 0x55AA00FF);
        if (h > kStoneBand - 4 || col.biome > 0.25f) {
            if (pick < 0.6f) stamp_conifer(sink, src.i, h + 1, src.k);
            else             stamp_oak(sink, src.i, h + 1, src.k);
        } else {
            if (pick < 0.15f)      stamp_conifer(sink, src.i, h + 1, src.k);
            else if (pick < 0.85f) stamp_oak(sink, src.i, h + 1, src.k);
            else                   stamp_bush(sink, src.i, h + 1, src.k);
        }
    }
    return pc;
}

PrismChunk build_prism_chunk_from_blocks(const Chunk& chunk, ChunkCoord coord,
                                         TerrainGen4D::Slice s) {
    ZoneScopedN("build_prism_chunk_from_blocks");
    PrismChunk pc = tile_footprint(coord, s);
    for (auto& cell : pc.cells) {
        int vx = 0, vz = 0;
        centroid_voxel(cell.poly, vx, vz);
        for (int y = 0; y < kChunkSizeY; ++y) {
            cell.blocks[static_cast<std::size_t>(y)] =
                static_cast<std::uint8_t>(chunk.get(vx, y, vz));
        }
    }
    return pc;
}

void apply_voxel_edit_to_cells(PrismChunk& pc, int x, int y, int z, BlockId b) {
    if (y < 0 || y >= kChunkSizeY) return;
    const SliceBasis basis = SliceBasis::from(pc.slice);
    const float x0 = static_cast<float>(pc.coord.x * kChunkSizeX);
    const float z0 = static_cast<float>(pc.coord.z * kChunkSizeZ);
    const CellKey want = cell_at(basis, x0 + static_cast<float>(x) + 0.5f,
                                 z0 + static_cast<float>(z) + 0.5f);
    for (auto& cell : pc.cells) {
        if (cell.i != want.i || cell.k != want.k || cell.l != want.l) continue;
        cell.blocks[static_cast<std::size_t>(y)] = static_cast<std::uint8_t>(b);
        return;
    }
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


// A horizontal face waiting to be merged: which lattice cell it belongs
// to, and everything that has to match for two of them to become one.
//
// Block id, light and AO are all in the key rather than averaged over a
// merge, so a merged face is exactly what the unmerged faces were. The
// cube mesher can afford to merge across differing light because it
// samples per corner and lets the gradient interpolate; a merged box here
// is one flat polygon with one value, so merging across a difference
// would flatten it. Block light is 0 almost everywhere - only a Glow
// block makes it otherwise - so this costs nothing on ordinary terrain
// and keeps a lit cave mouth honest.
struct CapKey {
    std::int32_t l, y;
    std::uint8_t block, dir, light, ao;
    bool operator==(const CapKey& o) const {
        return l == o.l && y == o.y && block == o.block && dir == o.dir &&
               light == o.light && ao == o.ao;
    }
};

struct CapKeyHash {
    std::size_t operator()(const CapKey& c) const noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(c.l) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint32_t>(c.y) * 0xBF58476D1CE4E5B9ull;
        h ^= (static_cast<std::uint64_t>(c.block) << 24) ^
             (static_cast<std::uint64_t>(c.dir)   << 16) ^
             (static_cast<std::uint64_t>(c.light) <<  8) ^
              static_cast<std::uint64_t>(c.ao);
        h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
        return static_cast<std::size_t>(h);
    }
};

// The greedy sweep, in lattice space.
//
// Identical in shape to the one the cube mesher runs over a voxel slice -
// take a run along one axis, then extend it along the other while every
// row matches - and that is the point. The mask here is indexed by (i, k)
// of the 4D lattice rather than by (x, z) of the chunk, which is the only
// difference and the whole trick: in lattice space the faces to be merged
// ARE a rectangular grid, however the slice is turned.
//
// Emits inclusive boxes [i0, i1] x [k0, k1].
template <typename Emit>
void greedy_merge_lattice(std::vector<std::pair<std::int32_t, std::int32_t>>& cells,
                          Emit&& emit) {
    std::sort(cells.begin(), cells.end());
    std::unordered_set<std::int64_t> live;
    live.reserve(cells.size() * 2);
    auto key = [](std::int32_t i, std::int32_t k) {
        return (static_cast<std::int64_t>(i) << 32) ^
               static_cast<std::uint32_t>(k);
    };
    for (const auto& c : cells) live.insert(key(c.first, c.second));

    for (const auto& c : cells) {
        const std::int32_t i0 = c.first, k0 = c.second;
        if (!live.count(key(i0, k0))) continue;

        std::int32_t k1 = k0;
        while (live.count(key(i0, k1 + 1))) ++k1;

        std::int32_t i1 = i0;
        for (;;) {
            const std::int32_t next = i1 + 1;
            bool whole_row = true;
            for (std::int32_t k = k0; k <= k1 && whole_row; ++k)
                if (!live.count(key(next, k))) whole_row = false;
            if (!whole_row) break;
            i1 = next;
        }

        for (std::int32_t i = i0; i <= i1; ++i)
            for (std::int32_t k = k0; k <= k1; ++k)
                live.erase(key(i, k));
        emit(i0, i1, k0, k1);
    }
}


// A vertical face waiting to be merged.
//
// The lattice face it lies on - which axis, which coordinate, which side -
// plus everything that has to match for two of them to become one wall:
// the block, its light and AO, and the exact y range the wall spans.
//
// The y range is in the key rather than merged over, and that is what
// keeps this honest. A wall is emitted for the y where its cell is solid
// and the cell across is not, so two neighbouring cells only share a wall
// where they share that whole run. Merging across differing runs would
// paper over the gap between them.
struct WallKey {
    std::int8_t  axis, side;      // 0=i 1=k 2=l ; 0=low face 1=high face
    std::int32_t face;            // the lattice coordinate of that face
    std::int32_t y0, y1;          // inclusive run the wall spans
    std::uint8_t block, light;
    bool operator==(const WallKey& o) const {
        return axis == o.axis && side == o.side && face == o.face &&
               y0 == o.y0 && y1 == o.y1 && block == o.block &&
               light == o.light;
    }
};

// One cell's contribution to a wall bucket: where it sits in the two
// lattice axes the face does not span, and the segment it would emit on
// its own.
//
// The segment is carried rather than recomputed because it is the
// fallback: a merged box normally yields one edge on the face, but a face
// that lies exactly along the chunk boundary can clip away, and then the
// members have to go out individually. Dropping them instead would be a
// hole in the world.
struct WallEntry {
    std::int32_t a, b;
    float ax, az, bx, bz;
};

struct WallKeyHash {
    std::size_t operator()(const WallKey& w) const noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(w.face) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint32_t>(w.y0) * 0xBF58476D1CE4E5B9ull;
        h ^= static_cast<std::uint32_t>(w.y1) * 0x94D049BB133111EBull;
        h ^= (static_cast<std::uint64_t>(w.axis)  << 40) ^
             (static_cast<std::uint64_t>(w.side)  << 32) ^
             (static_cast<std::uint64_t>(w.block) <<  8) ^
              static_cast<std::uint64_t>(w.light);
        h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
        return static_cast<std::size_t>(h);
    }
};

// Which lattice face a polygon edge lies on, if any.
//
// The mesher never tagged its edges - the clipper produces a polygon and
// nothing records which half-plane cut which side - so the face is
// recovered from the geometry: an edge lies on a face when BOTH of its
// endpoints satisfy that face's plane equation. Cheap (six dot products
// per endpoint) and it cannot disagree with the clipper, because it asks
// the same linear forms the clipper was built from.
//
// Returns false for an edge on the chunk's own boundary, which lies on no
// lattice face and is emitted unmerged.
inline bool edge_face(const SliceBasis& b, int i, int k, int l,
                      float ax, float az, float bx, float bz,
                      float x0, float z0,
                      std::int8_t* axis, std::int8_t* side, std::int32_t* face) {
    const float fx[3] = {b.x4_sx, b.z4_sx, b.w4_sx};
    const float fz[3] = {b.x4_sz, b.z4_sz, b.w4_sz};
    const float fc[3] = {b.x4_c,  b.z4_c,  b.w4_c};
    const int lo[3] = {i, k, l};
    // Chunk-local in, patch coordinates out: the forms are defined on the
    // patch and the polygon was stored relative to the chunk origin.
    const float pax = ax + x0, paz = az + z0;
    const float pbx = bx + x0, pbz = bz + z0;
    for (int a = 0; a < 3; ++a) {
        const float va = fx[a] * pax + fz[a] * paz + fc[a];
        const float vb = fx[a] * pbx + fz[a] * pbz + fc[a];
        for (int sd = 0; sd < 2; ++sd) {
            const float plane = static_cast<float>(lo[a]) + (sd ? 1.0f : 0.0f);
            if (std::fabs(va - plane) < 1e-3f && std::fabs(vb - plane) < 1e-3f) {
                *axis = static_cast<std::int8_t>(a);
                *side = static_cast<std::int8_t>(sd);
                *face = lo[a];
                return true;
            }
        }
    }
    return false;
}

struct PrismVertex {
    float x, y, z;
    float u, v;
};

void push_quad(ChunkMeshData& out, const PrismVertex q[4], std::uint8_t normal,
               std::uint8_t id, std::uint8_t light, std::uint8_t ao) {
    for (int i = 0; i < 4; ++i) {
        gfx::VertexPacked p;
        p.x = gfx::quantize_sub_unit_xz(q[i].x);
        p.z = gfx::quantize_sub_unit_xz(q[i].z);
        p.y = static_cast<std::uint16_t>(q[i].y);
        p.normal = normal;
        p.ao = ao;
        p.u = gfx::quantize_sub_unit_uv(q[i].u);
        p.v = gfx::quantize_sub_unit_uv(q[i].v);
        p.block_id = id;
        p.light = light;
        out.vertices.push_back(p);
    }
    ++out.quad_count;
}

}  // namespace

ChunkMeshData build_prism_mesh(const PrismChunk& pc,
                               const NeighborPlanes& neighbors,
                               const LightSource& light) {
    ZoneScopedN("build_prism_mesh");
    const auto t0 = std::chrono::steady_clock::now();

    ChunkMeshData out;
    out.xz_scale = gfx::kSubUnitXZScale;
    out.uv_scale = gfx::kSubUnitUVScale;

    // Normal indices 2 and 3 are +Y and -Y in the packed table.
    constexpr std::uint8_t kUp = 2, kDown = 3;

    // Whether the chunk next door hides a wall that sits on this chunk's
    // own boundary.
    //
    // The neighbour is held as a voxel layer, not as cells, so this is a
    // grid answer to a lattice question and it is deliberately biased:
    // the wall is hidden only if EVERY voxel the segment touches is
    // solid. One air voxel anywhere along it keeps the wall. A cell
    // narrower than a voxel could still fall between two solid voxels and
    // lose a wall it needed, and the artifact would be a gap thinner than
    // a block; that is the residual, and it is the same order as the
    // rasterisation error physics already carries.
    //
    // An edge with no cell across it is not necessarily a chunk boundary
    // - it can also be an edge whose neighbouring cell was too thin to
    // keep - so the boundary is identified from where the outward step
    // actually lands, not from the missing index.
    auto boundary_hides = [&](float ax, float az, float bx, float bz,
                              float nx, float nz, int at_y) {
        const float mx = (ax + bx) * 0.5f + nx * 0.01f;
        const float mz = (az + bz) * 0.5f + nz * 0.01f;
        const BoundaryPlane* plane = nullptr;
        float t0 = 0.0f, t1 = 0.0f;
        if (mx < 0.0f) {
            plane = &neighbors.neg_x; t0 = std::min(az, bz); t1 = std::max(az, bz);
        } else if (mx > static_cast<float>(kChunkSizeX)) {
            plane = &neighbors.pos_x; t0 = std::min(az, bz); t1 = std::max(az, bz);
        } else if (mz < 0.0f) {
            plane = &neighbors.neg_z; t0 = std::min(ax, bx); t1 = std::max(ax, bx);
        } else if (mz > static_cast<float>(kChunkSizeZ)) {
            plane = &neighbors.pos_z; t0 = std::min(ax, bx); t1 = std::max(ax, bx);
        } else {
            return false;
        }
        if (!plane->present) return false;
        const int lo = std::clamp(static_cast<int>(std::floor(t0)),
                                  0, kChunkSizeX - 1);
        const int hi = std::clamp(static_cast<int>(std::ceil(t1)) - 1,
                                  0, kChunkSizeX - 1);
        for (int t = lo; t <= hi; ++t)
            if (!is_solid(plane->at(t, at_y))) return false;
        return true;
    };

    std::unordered_map<CapKey, std::vector<std::pair<std::int32_t, std::int32_t>>,
                       CapKeyHash> caps;
    std::unordered_map<WallKey, std::vector<WallEntry>, WallKeyHash> walls;

    const SliceBasis basis = SliceBasis::from(pc.slice);
    const float px = static_cast<float>(pc.coord.x * kChunkSizeX);
    const float pz = static_cast<float>(pc.coord.z * kChunkSizeZ);

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

            // Light is read from the cell OUTSIDE the face, which is the
            // lit one - the same rule the cube mesher uses. A face lit
            // from its own solid interior would read black everywhere.
            //
            // Per face rather than per vertex. The greedy mesher samples
            // each corner of a merged rectangle so a long quad running
            // out of a cave gradates; a prism cap is one cell across, so
            // there is nothing for four samples to gradate between and
            // the extra three lookups would buy nothing.
            const int vx = cell.vx, vz = cell.vz;

            // Ambient occlusion, per face and derived from the tiling
            // rather than from a 3x3 voxel stencil.
            //
            // The voxel formula asks which of the eight cells around a
            // corner are solid; a cell here has between three and six
            // neighbours and no corners in common with a grid, so that
            // formula has nothing to index. What carries over is the
            // meaning: a face is darker the more enclosed it is. Counting
            // how many of the cell's own neighbours are solid at the
            // face's own height is the same statement in the tiling's
            // terms, and it darkens the inside of a pit, a crevice and a
            // cave mouth exactly where the voxel version does.
            auto enclosure_ao = [&](int at_y) {
                int walls = 0;
                for (int e = 0; e < n; ++e) {
                    const std::int32_t o = cell.across[e];
                    if (o < 0) continue;
                    if (solid(pc.cells[static_cast<std::size_t>(o)]
                                  .blocks[static_cast<std::size_t>(at_y)])) ++walls;
                }
                if (walls >= n)     return 0;
                if (walls >= n - 1) return 1;
                if (walls >= n - 2) return 2;
                return 3;
            };

            // Horizontal faces are COLLECTED here and merged after the
            // sweep, rather than emitted one polygon per cell. See the
            // merge pass below: a lattice box presents a convex polygon,
            // so the ordinary greedy rectangle sweep applies - it just
            // has to run over (i, k) of the lattice instead of (x, z) of
            // the chunk.
            if (cap_top) {
                caps[CapKey{cell.l, y1 + 1, id, 0,
                            sample_light(light, vx, y1 + 1, vz),
                            static_cast<std::uint8_t>(enclosure_ao(
                                std::min(y1 + 1, kChunkSizeY - 1)))}]
                    .emplace_back(cell.i, cell.k);
            }
            if (cap_bottom) {
                caps[CapKey{cell.l, y, id, 1,
                            sample_light(light, vx, y - 1, vz),
                            static_cast<std::uint8_t>(enclosure_ao(
                                std::max(y - 1, 0)))}]
                    .emplace_back(cell.i, cell.k);
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
                        if (other != nullptr)
                            return solid(other->blocks[static_cast<std::size_t>(yy)]);
                        return boundary_hides(ax, az, bx, bz, dz / len, -dx / len, yy);
                    };
                    if (hidden(wy)) { ++wy; continue; }
                    int wy1 = wy;
                    while (wy1 + 1 <= y1 && !hidden(wy1 + 1)) ++wy1;
                    const float lo = static_cast<float>(wy);
                    const float hi = static_cast<float>(wy1 + 1);
                    // The air is on the other side of the wall, so that
                    // is where the light is. With no cell across (the
                    // chunk boundary) fall back to this cell's own
                    // column, which is the neighbour's light one block
                    // away and the best guess available without holding
                    // the neighbouring chunk.
                    const int lx = other ? other->vx : vx;
                    const int lz = other ? other->vz : vz;
                    const std::uint8_t lt =
                        sample_light(light, lx, (wy + wy1) / 2, lz);
                    // Which lattice face this edge lies on.
                    //
                    // Taken from the NEIGHBOUR when there is one, because
                    // that is exact: the cell across an edge differs by
                    // exactly one on exactly one axis, and that names the
                    // face outright. Deriving it from the geometry
                    // instead was the first attempt and it silently did
                    // almost nothing at a tilt - the polygon has been
                    // snapped to the vertex grid by then, which moves a
                    // plane equation by up to half a quantisation step,
                    // thirty times the tolerance the test used. Flat cuts
                    // snap exactly, so it worked there and only there.
                    std::int8_t axis = -1, side = 0;
                    std::int32_t face = 0;
                    const std::int32_t lat[3] = {cell.i, cell.k, cell.l};
                    if (other != nullptr) {
                        const std::int32_t d[3] = {other->i - cell.i,
                                                   other->k - cell.k,
                                                   other->l - cell.l};
                        for (int a2 = 0; a2 < 3; ++a2) {
                            if (d[a2] == 1)      { axis = static_cast<std::int8_t>(a2); side = 1; }
                            else if (d[a2] == -1){ axis = static_cast<std::int8_t>(a2); side = 0; }
                        }
                    }
                    if (axis >= 0) {
                        face = lat[axis];
                        const int other0 = (axis == 0) ? 1 : 0;
                        const int other1 = (axis == 2) ? 1 : 2;
                        walls[WallKey{axis, side, face, wy, wy1, id, lt}]
                            .push_back(WallEntry{lat[other0], lat[other1],
                                                 ax, az, bx, bz});
                    } else {
                        q[0] = {ax, lo, az, 0.0f,  lo};
                        q[1] = {ax, hi, az, 0.0f,  hi};
                        q[2] = {bx, hi, bz, len,   hi};
                        q[3] = {bx, lo, bz, len,   lo};
                        push_quad(out, q, nrm, id, lt, 3);
                    }
                    wy = wy1 + 1;
                }
            }

            y = y1 + 1;
        }
    }

    // The merge. Each bucket is one horizontal plane of one block at one
    // light and AO level, as a set of (i, k) lattice cells; the greedy
    // sweep turns that into maximal boxes and each box becomes one convex
    // polygon.
    //
    // Snapped with the same function the cell tiling uses, because a
    // merged cap's edge has to land exactly where the wall hanging off it
    // does. Both are the same coordinate; the same coordinate snaps the
    // same way.
    for (auto& bucket : caps) {
        const CapKey& k = bucket.first;
        greedy_merge_lattice(bucket.second,
                             [&](std::int32_t i0, std::int32_t i1,
                                 std::int32_t k0, std::int32_t k1) {
            SlicePolygon poly = box_polygon(i0, i1, k0, k1, k.l, basis,
                                            px, pz, static_cast<float>(kChunkSizeX));
            SlicePolygon snapped;
            for (int v = 0; v < poly.count; ++v) {
                const float sx = snap_xz(poly.x[v] - px);
                const float sz = snap_xz(poly.z[v] - pz);
                if (snapped.count > 0 &&
                    snapped.x[snapped.count - 1] == sx &&
                    snapped.z[snapped.count - 1] == sz) continue;
                snapped.x[snapped.count] = sx;
                snapped.z[snapped.count] = sz;
                ++snapped.count;
            }
            while (snapped.count > 1 &&
                   snapped.x[0] == snapped.x[snapped.count - 1] &&
                   snapped.z[0] == snapped.z[snapped.count - 1]) --snapped.count;
            if (snapped.count < 3) return;

            const int m = snapped.count;
            const float plane_y = static_cast<float>(k.y);
            const bool top = (k.dir == 0);
            fan_quads(m, [&](int a, int b, int c, int d) {
                // The polygon is positively wound, which is the -Y
                // winding this engine uses; +Y wants it reversed.
                const int in[4] = {a, b, c, d};
                for (int v = 0; v < 4; ++v) {
                    const int idx = top ? (m - 1 - in[v]) : in[v];
                    q[v] = {snapped.x[idx], plane_y, snapped.z[idx],
                            snapped.x[idx], snapped.z[idx]};
                }
                push_quad(out, q, top ? kUp : kDown, k.block, k.light, k.ao);
            });
        });
    }

    // The wall merge. Each bucket is one lattice face, one block, one
    // light, one y run; its members are the cells along that face in the
    // two lattice axes the face does not span.
    //
    // The merged wall's footprint is an EDGE of the merged box's polygon -
    // the one lying on the face - which is why this can reuse box_polygon
    // rather than clip a line by hand. The edge is found the same way the
    // bucket key was: an edge lies on a face when both endpoints satisfy
    // that face's plane.
    for (auto& bucket : walls) {
        const WallKey& w = bucket.first;
        const int other0 = (w.axis == 0) ? 1 : 0;
        const int other1 = (w.axis == 2) ? 1 : 2;

        // Emits one wall quad from a segment already in chunk-local,
        // snapped coordinates.
        auto emit_wall = [&](float sax, float saz, float sbx, float sbz) {
            const float dx = sbx - sax, dz = sbz - saz;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-4f) return;
            const float lo = static_cast<float>(w.y0);
            const float hi = static_cast<float>(w.y1 + 1);
            q[0] = {sax, lo, saz, 0.0f, lo};
            q[1] = {sax, hi, saz, 0.0f, hi};
            q[2] = {sbx, hi, sbz, len,  hi};
            q[3] = {sbx, lo, sbz, len,  lo};
            push_quad(out, q, gfx::encode_horizontal_normal(dz, -dx),
                      w.block, w.light, 3);
        };

        std::vector<std::pair<std::int32_t, std::int32_t>> coords;
        coords.reserve(bucket.second.size());
        for (const auto& e : bucket.second) coords.emplace_back(e.a, e.b);

        greedy_merge_lattice(coords, [&](std::int32_t a0, std::int32_t a1,
                                         std::int32_t b0, std::int32_t b1) {
            std::int32_t lo3[3], hi3[3];
            lo3[w.axis] = hi3[w.axis] = w.face;
            lo3[other0] = a0; hi3[other0] = a1;
            lo3[other1] = b0; hi3[other1] = b1;

            const SlicePolygon poly =
                box_polygon(lo3[0], hi3[0], lo3[1], hi3[1], lo3[2], hi3[2],
                            basis, px, pz, static_cast<float>(kChunkSizeX));

            // The merged wall is the edge of the merged box that lies on
            // the face. Found on the UNSNAPPED box polygon, where the
            // plane test is exact - the snap happens after.
            if (!poly.empty()) {
                for (int e = 0; e < poly.count; ++e) {
                    const int f = (e + 1) % poly.count;
                    std::int8_t ax2 = 0, sd2 = 0;
                    std::int32_t fc2 = 0;
                    if (!edge_face(basis, lo3[0], lo3[1], lo3[2],
                                   poly.x[e] - px, poly.z[e] - pz,
                                   poly.x[f] - px, poly.z[f] - pz,
                                   px, pz, &ax2, &sd2, &fc2)) continue;
                    if (ax2 != w.axis || sd2 != w.side) continue;
                    emit_wall(snap_xz(poly.x[e] - px), snap_xz(poly.z[e] - pz),
                              snap_xz(poly.x[f] - px), snap_xz(poly.z[f] - pz));
                    return;
                }
            }

            // No edge on the face. This should be unreachable, and the
            // argument is short: every member of this bucket contributed
            // an edge that lies on the face AND inside the patch, and the
            // merged box contains every member, so the box's own
            // intersection with the face plane is non-empty inside the
            // patch and has to show up as an edge.
            //
            // Instrumented rather than assumed - it fired zero times
            // across the whole test suite, including the sixty-case fuzz.
            // So it is an assert in debug and the safe direction in
            // release: emit the members individually rather than drop
            // them, because a dropped wall is a hole in the world and a
            // duplicated one is a wasted quad.
            assert(false && "merged wall box has no edge on its own face");
            for (const auto& e : bucket.second) {
                if (e.a < a0 || e.a > a1 || e.b < b0 || e.b > b1) continue;
                emit_wall(e.ax, e.az, e.bx, e.bz);
            }
        });
    }

    out.build_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace world
