#pragma once

#include "world/terrain_gen4d.h"

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
// shape a block presents is at most a hexagonal pillar. That is the
// phrase the 4D Miner wiki uses, and it falls out of the algebra rather
// than needing to be aimed at.
struct SlicePolygon {
    // Six half-planes can produce at most six vertices; the extra slot
    // lets the clipper carry an intermediate result without a branch.
    static constexpr int kMaxVerts = 8;
    std::array<float, kMaxVerts> x{};
    std::array<float, kMaxVerts> z{};
    int count = 0;

    bool empty() const { return count < 3; }
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
    return p;
}

}  // namespace world
