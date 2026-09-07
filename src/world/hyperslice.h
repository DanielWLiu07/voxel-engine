#pragma once

#include "world/terrain_gen4d.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace world {

// The shape a 4D block presents to a tilted 3D slice.
//
// This engine's normal path renders blocks as cubes, and for a cut turned
// in ONE plane that is exact: the slice stays square-on to the two axes
// it does not turn in, so every cross-section is an eight-vertex box.
// Turn both planes at once and it stops being true - a unit tesseract cut
// at a compound angle presents six-, eight- and ten-vertex polytopes,
// which is the look 4D Miner is known for.
//
// The general case sounds like a convex-hull problem in 4D. It is not,
// and the reason is worth stating because it makes the whole thing cheap:
// to_4d is linear in (sx, sz) and passes sy straight through, so the six
// constraints that place a point inside one lattice cell
//
//     i <= x4 < i+1,  k <= z4 < k+1,  l <= w4 < l+1
//
// do not involve sy at all. The preimage of a cell is therefore a convex
// POLYGON in (sx, sz) extruded through a unit interval in y - a prism.
// Six half-planes bound a convex polygon of at most six sides, so the
// shape a block presents is at most a hexagonal pillar.
//
// "Hexagonal pillar" was attributed to the 4D Miner wiki here, and that
// was not checked. The wiki says "uniquely-shaped slices of 4D blocks"
// and does not name a shape, so the attribution is withdrawn - the
// result stands on the algebra above, which is where it came from.
//
// What the wiki DOES confirm is the premise the prism argument rests on:
// its slice rotates in the ZW plane (mouse wheel, or vertical mouse with
// M held) and the XW plane (horizontal mouse with M held), and in no
// other. y is never mixed with w in either game. That is not a
// simplification on this engine's part - it is what makes a cell's
// cross-section a prism rather than a general polytope, and it is why
// vertical runs of blocks merge exactly.
struct SlicePolygon {
    // Ten, and the arithmetic matters because the clipper truncates
    // SILENTLY when it runs out of room - it would drop vertices and
    // hand back a polygon that is simply the wrong shape.
    //
    // A cell or a box is bounded by six lattice half-planes, and the
    // patch square adds four more. The intersection of a convex region
    // bounded by six lines with a rectangle therefore has at most ten
    // sides. This was 8, which was the count for the lattice alone: it
    // held for single cells because a unit cell can only reach one
    // corner of a 16-wide patch, and it stops holding the moment a
    // merged BOX spans enough of the patch to touch two edges at once.
    // Two spare slots on top, so the clipper's intermediate result never
    // has to be reasoned about.
    static constexpr int kMaxVerts = 12;
    std::array<float, kMaxVerts> x{};
    std::array<float, kMaxVerts> z{};
    int count = 0;

    bool empty() const { return count < 3; }

    // The bound above, as an assertion the tests can make. Ten sides is
    // the ceiling for anything this file produces; anything more means
    // the clipper was handed something that is not a lattice region.
    static constexpr int kMaxSides = 10;
};

// The three linear forms that carry a slice point into the noise lattice.
// Precomputed per slice rather than per cell: they do not depend on which
// cell is being tested, and recomputing four sines per cell would dominate
// a mesher that visits tens of thousands of them.
struct SliceBasis {
    float x4_sx, x4_sz, x4_c;
    float z4_sx, z4_sz, z4_c;
    float w4_sx, w4_sz, w4_c;

    static SliceBasis from(TerrainGen4D::Slice s) {
        const float W = s.w * kWScale;
        const float cp = std::cos(s.phi), sp = std::sin(s.phi);
        const float ct = std::cos(s.theta), st = std::sin(s.theta);
        // x4 = (sx + x_shift) cos(phi) - W sin(phi)
        // w1 = (sx + x_shift) sin(phi) + W cos(phi)
        const float b0 = s.x_shift * sp + W * cp;
        SliceBasis b{};
        b.x4_sx = cp;            b.x4_sz = 0.0f;  b.x4_c = s.x_shift * cp - W * sp;
        b.z4_sx = -sp * st;      b.z4_sz = ct;    b.z4_c = s.z_shift * ct - b0 * st;
        b.w4_sx =  sp * ct;      b.w4_sz = st;    b.w4_c = s.z_shift * st + b0 * ct;
        return b;
    }
};

namespace detail {

// Sutherland-Hodgman against a single half-plane a*x + b*z <= c.
inline void clip_half_plane(SlicePolygon& p, float a, float b, float c) {
    SlicePolygon out;
    for (int i = 0; i < p.count; ++i) {
        const int j = (i + 1) % p.count;
        const float di = a * p.x[i] + b * p.z[i] - c;
        const float dj = a * p.x[j] + b * p.z[j] - c;
        const bool in_i = di <= 0.0f, in_j = dj <= 0.0f;
        if (in_i && out.count < SlicePolygon::kMaxVerts) {
            out.x[out.count] = p.x[i];
            out.z[out.count] = p.z[i];
            ++out.count;
        }
        if (in_i != in_j && out.count < SlicePolygon::kMaxVerts) {
            const float t = di / (di - dj);
            out.x[out.count] = p.x[i] + t * (p.x[j] - p.x[i]);
            out.z[out.count] = p.z[i] + t * (p.z[j] - p.z[i]);
            ++out.count;
        }
    }
    p = out;
}

}  // namespace detail

// The polygon a lattice cell presents, in slice (sx, sz) coordinates,
// clipped to the square [x0, x0+span] x [z0, z0+span].
//
// The bounding square is not a detail: without it the preimage of a cell
// is unbounded whenever the slice is parallel to one of the cell's faces,
// which happens for every cell at theta = phi = 0 and for a whole family
// of them at any axis-aligned angle.
// The polygon a BOX of lattice cells presents - [i0, i1] x [k0, k1] at
// w slab l, inclusive - clipped to the patch.
//
// This is what makes greedy meshing work on a tilted cut, and the reason
// is one line of algebra: the preimage of a convex set under a linear map
// is convex. to_4d is linear, and a lattice box is convex, so the region
// a box presents to the slice is a convex polygon - always, at any angle,
// however many cells it spans. It fan-triangulates exactly like a single
// cell's polygon and it is still bounded by six half-planes, so it still
// has at most six sides.
//
// The prism mesher's own comments used to say caps could not be merged,
// because "neighbouring cells present their own polygons at their own
// angles". True of the polygons and false of the conclusion: two cells
// that share a lattice face present two polygons that share an edge, and
// their union is the preimage of the two-cell box. Merging just has to
// happen in LATTICE space rather than in slice space, where the mask is
// rectangular and the ordinary greedy sweep applies unchanged.
//
// At a flat cut the lattice IS the voxel grid, so this reduces to the
// engine's existing greedy mesher exactly.
inline SlicePolygon box_polygon(int i0, int i1, int k0, int k1, int l,
                                const SliceBasis& b,
                                float x0, float z0, float span) {
    SlicePolygon p;
    p.count = 4;
    p.x = {x0, x0 + span, x0 + span, x0, 0, 0, 0, 0};
    p.z = {z0, z0, z0 + span, z0 + span, 0, 0, 0, 0};

    const float lo[3] = {static_cast<float>(i0), static_cast<float>(k0),
                         static_cast<float>(l)};
    const float hi[3] = {static_cast<float>(i1) + 1.0f,
                         static_cast<float>(k1) + 1.0f,
                         static_cast<float>(l) + 1.0f};
    const float ax[3] = {b.x4_sx, b.z4_sx, b.w4_sx};
    const float az[3] = {b.x4_sz, b.z4_sz, b.w4_sz};
    const float ac[3] = {b.x4_c,  b.z4_c,  b.w4_c};
    for (int axis = 0; axis < 3; ++axis) {
        detail::clip_half_plane(p, -ax[axis], -az[axis], ac[axis] - lo[axis]);
        detail::clip_half_plane(p,  ax[axis],  az[axis], hi[axis] - ac[axis]);
        if (p.empty()) return p;
    }
    return p;
}

inline SlicePolygon cell_polygon(int i, int k, int l, const SliceBasis& b,
                                 float x0, float z0, float span) {
    SlicePolygon p;
    p.count = 4;
    p.x = {x0, x0 + span, x0 + span, x0, 0, 0, 0, 0};
    p.z = {z0, z0, z0 + span, z0 + span, 0, 0, 0, 0};

    const float lo[3] = {static_cast<float>(i), static_cast<float>(k),
                         static_cast<float>(l)};
    const float ax[3] = {b.x4_sx, b.z4_sx, b.w4_sx};
    const float az[3] = {b.x4_sz, b.z4_sz, b.w4_sz};
    const float ac[3] = {b.x4_c,  b.z4_c,  b.w4_c};
    for (int axis = 0; axis < 3; ++axis) {
        // lo <= a.sx + b.sz + c  ->  -a.sx - b.sz <= c - lo
        detail::clip_half_plane(p, -ax[axis], -az[axis], ac[axis] - lo[axis]);
        //        a.sx + b.sz + c <= lo + 1
        detail::clip_half_plane(p,  ax[axis],  az[axis], lo[axis] + 1.0f - ac[axis]);
        if (p.empty()) return p;
    }

    // Ownership, decided on the half-open cell [lo, lo+1).
    //
    // The clip above has to be CLOSED or adjacent cells would not share
    // their edge and the world would come apart along every cell
    // boundary. Closed clipping cannot decide who owns a point that lies
    // exactly on a shared face, and there is one slice where that is not
    // a measure-zero curiosity but the common case: at theta = phi = 0
    // with w = 0 the cut lies exactly ON the lattice plane w4 = 0, so
    // cells l = -1 and l = 0 BOTH clip to the full square and every
    // column of the world is drawn twice. That is the engine's default
    // slice, so the degenerate case is the one it starts in.
    //
    // Deciding on the centroid rather than by nudging the slice keeps
    // the geometry exact and costs one dot product per axis: a convex
    // polygon with area is strictly inside its own hull, so the vertex
    // average is an interior point, and an interior point satisfies the
    // half-open test for exactly one cell per axis.
    float cx = 0.0f, cz = 0.0f;
    for (int v = 0; v < p.count; ++v) { cx += p.x[v]; cz += p.z[v]; }
    cx /= static_cast<float>(p.count);
    cz /= static_cast<float>(p.count);
    for (int axis = 0; axis < 3; ++axis) {
        const float v = ax[axis] * cx + az[axis] * cz + ac[axis];
        if (v < lo[axis] || v >= lo[axis] + 1.0f) {
            p.count = 0;
            return p;
        }
    }
    return p;
}


// The area a polygon covers in the slice, by the shoelace formula.
// Positive for the counter-clockwise winding the clipper preserves.
inline float polygon_area(const SlicePolygon& p) {
    if (p.empty()) return 0.0f;
    float a = 0.0f;
    for (int i = 0; i < p.count; ++i) {
        const int j = (i + 1) % p.count;
        a += p.x[i] * p.z[j] - p.x[j] * p.z[i];
    }
    return 0.5f * a;
}

// The lattice cells that can possibly meet a square patch of the slice.
//
// Each of the three linear forms is affine in (sx, sz), so its range over
// an axis-aligned square is attained at the corners and costs two
// comparisons rather than a search. The bounds are inclusive and
// deliberately loose by up to one cell per axis: cell_polygon is the
// authority on whether a cell is actually met, and widening the box costs
// one clip that returns empty while narrowing it would drop geometry.
struct CellRange {
    int lo[3] = {0, 0, 0};
    int hi[3] = {0, 0, 0};

    std::size_t volume() const {
        std::size_t v = 1;
        for (int a = 0; a < 3; ++a)
            v *= static_cast<std::size_t>(hi[a] - lo[a] + 1);
        return v;
    }
};

inline CellRange cells_over_patch(const SliceBasis& b, float x0, float z0,
                                  float span) {
    const float ax[3] = {b.x4_sx, b.z4_sx, b.w4_sx};
    const float az[3] = {b.x4_sz, b.z4_sz, b.w4_sz};
    const float ac[3] = {b.x4_c,  b.z4_c,  b.w4_c};
    CellRange r;
    for (int a = 0; a < 3; ++a) {
        const float at_origin = ax[a] * x0 + az[a] * z0 + ac[a];
        const float dx = ax[a] * span;
        const float dz = az[a] * span;
        const float lo = at_origin + std::min(0.0f, dx) + std::min(0.0f, dz);
        const float hi = at_origin + std::max(0.0f, dx) + std::max(0.0f, dz);
        r.lo[a] = static_cast<int>(std::floor(lo));
        r.hi[a] = static_cast<int>(std::floor(hi));
    }
    return r;
}

// Every cell that actually meets the patch, with the polygon it presents.
//
// The callback shape is (i, k, l, polygon). Cells are visited in lattice
// order, which is not slice order - a mesher that cares about locality
// should sort what comes out, not expect this to hand it a sweep.
//
// This is the brute-force walk of the bounding box, and it stays that way
// on purpose: a 16x16 patch at a compound tilt has a box of a few
// thousand cells against the ~65k noise samples the same patch costs to
// generate, so the clip is noise next to the field it is tiling. If that
// ever stops being true the fix is a seeded flood fill across shared
// edges, not a cleverer box.
template <typename Fn>
void for_each_cell(const SliceBasis& b, float x0, float z0, float span,
                   Fn&& fn) {
    const CellRange r = cells_over_patch(b, x0, z0, span);
    for (int i = r.lo[0]; i <= r.hi[0]; ++i)
        for (int k = r.lo[1]; k <= r.hi[1]; ++k)
            for (int l = r.lo[2]; l <= r.hi[2]; ++l) {
                const SlicePolygon p = cell_polygon(i, k, l, b, x0, z0, span);
                if (!p.empty()) fn(i, k, l, p);
            }
}

}  // namespace world
