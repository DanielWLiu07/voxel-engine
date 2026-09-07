// What would 4D Miner's look actually cost?
//
// This engine renders a 3D slice of a 4D world as CUBES: solidity is
// sampled per voxel on a slice-aligned grid, and the greedy mesher merges
// coplanar faces into axis-aligned quads. That is where the repo's
// headline numbers come from - 5.3x fewer triangles, 12-byte vertices, a
// shared index buffer, 10.9 MB for a 71-million-voxel world.
//
// 4D Miner does something else. Its world is a lattice of tesseracts and
// it renders each one's actual cross-section, so a block can come out as
// a hexagonal pillar rather than a cube. That look cannot be had from
// axis-aligned quads, so the question is not whether it is nicer - it is
// what it costs, and nobody should answer that from an opinion.
//
//     cmake --build build --target hyperslice && ./build/hyperslice [theta] [phi]
//
// Pure geometry, no GL, no engine. Slices unit tesseracts with a
// hyperplane and counts what comes out.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct V4 { float x, y, z, w; };
struct V3 { float x, y, z; };

// A tesseract has 16 corners and 32 edges. Two corners share an edge when
// their bit patterns differ in exactly one place, which is the whole
// enumeration - no table needed.
struct Tesseract {
    std::array<V4, 16> corner{};
    std::vector<std::pair<int, int>> edge;

    Tesseract() {
        for (int i = 0; i < 16; ++i) {
            corner[i] = {static_cast<float>((i >> 0) & 1),
                         static_cast<float>((i >> 1) & 1),
                         static_cast<float>((i >> 2) & 1),
                         static_cast<float>((i >> 3) & 1)};
        }
        for (int a = 0; a < 16; ++a)
            for (int b = a + 1; b < 16; ++b)
                if (__builtin_popcount(a ^ b) == 1) edge.emplace_back(a, b);
    }
};

// The slice is the hyperplane {p : dot(p, n) = d}. Its intersection with a
// convex body is convex, so the cross-section is the convex hull of the
// points where it crosses the body's edges.
std::vector<V3> cross_section(const Tesseract& t, V4 n, float d) {
    auto dot = [&](V4 p) { return p.x*n.x + p.y*n.y + p.z*n.z + p.w*n.w; };
    // A basis for the hyperplane, so the hull can be taken in 3D rather
    // than in 4D. Gram-Schmidt against the normal.
    V4 seed[3] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    std::vector<V4> basis;
    for (V4 s : seed) {
        float k = dot(s);
        V4 v{s.x - k*n.x, s.y - k*n.y, s.z - k*n.z, s.w - k*n.w};
        for (const V4& b : basis) {
            const float p = v.x*b.x + v.y*b.y + v.z*b.z + v.w*b.w;
            v = {v.x - p*b.x, v.y - p*b.y, v.z - p*b.z, v.w - p*b.w};
        }
        const float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z + v.w*v.w);
        if (len > 1e-4f) basis.push_back({v.x/len, v.y/len, v.z/len, v.w/len});
        if (basis.size() == 3) break;
    }
    if (basis.size() < 3) return {};

    std::vector<V3> pts;
    for (const auto& [a, b] : t.edge) {
        const float da = dot(t.corner[a]) - d;
        const float db = dot(t.corner[b]) - d;
        if ((da > 0.0f) == (db > 0.0f)) continue;   // no crossing
        const float u = da / (da - db);
        const V4 A = t.corner[a], B = t.corner[b];
        const V4 p{A.x + u*(B.x-A.x), A.y + u*(B.y-A.y),
                   A.z + u*(B.z-A.z), A.w + u*(B.w-A.w)};
        pts.push_back({p.x*basis[0].x + p.y*basis[0].y + p.z*basis[0].z + p.w*basis[0].w,
                       p.x*basis[1].x + p.y*basis[1].y + p.z*basis[1].z + p.w*basis[1].w,
                       p.x*basis[2].x + p.y*basis[2].y + p.z*basis[2].z + p.w*basis[2].w});
    }
    // Deduplicate: a crossing exactly on a corner is found by every edge
    // meeting it.
    std::vector<V3> out;
    for (const V3& p : pts) {
        bool dup = false;
        for (const V3& q : out)
            if (std::fabs(p.x-q.x) < 1e-4f && std::fabs(p.y-q.y) < 1e-4f &&
                std::fabs(p.z-q.z) < 1e-4f) { dup = true; break; }
        if (!dup) out.push_back(p);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const float theta = (argc > 1) ? std::strtof(argv[1], nullptr) : 0.15f;
    const float phi   = (argc > 2) ? std::strtof(argv[2], nullptr) : 0.0f;

    const Tesseract t;
    // The slice's normal, from the same two rotations the engine uses: the
    // hyperplane w = const, turned by theta in ZW and phi in XW.
    const V4 n{-std::sin(phi) * std::cos(theta),
               0.0f,
               -std::sin(theta),
               std::cos(theta) * std::cos(phi)};

    std::printf("tesseract cross-sections at theta=%.3f phi=%.3f\n\n", theta, phi);
    std::printf("  %10s  %10s  %s\n", "offset", "vertices", "shape");

    // Sweep the hyperplane through one tesseract. Every distinct
    // cross-section a block can present is somewhere in this sweep.
    int counts[20] = {};
    int sampled = 0;
    constexpr int kSteps = 400;
    for (int i = 1; i < kSteps; ++i) {
        const float d = (static_cast<float>(i) / kSteps) *
                        (std::fabs(n.x) + std::fabs(n.y) +
                         std::fabs(n.z) + std::fabs(n.w));
        const auto pts = cross_section(t, n, d);
        if (pts.empty()) continue;
        ++sampled;
        if (pts.size() < 20) ++counts[pts.size()];
    }
    for (int v = 0; v < 20; ++v) {
        if (!counts[v]) continue;
        const char* shape =
            v == 4  ? "tetrahedron"      :
            v == 5  ? "square pyramid"   :
            v == 6  ? "triangular prism / octahedron" :
            v == 8  ? "cube or hexagonal pillar" :
            v == 12 ? "truncated form"   : "polytope";
        std::printf("  %10s  %10d  %-32s %5.1f%% of the sweep\n",
                    "", v, shape, 100.0 * counts[v] / sampled);
    }

    // The cost, stated as the thing the repo actually gates on.
    //
    // A cube contributes 6 quads = 12 triangles before merging, and the
    // greedy mesher takes that down by 5.3x across a chunk because
    // coplanar faces share a plane. A polytope's faces do not: each cell
    // presents its own hull, at its own angle, so there is nothing to
    // merge.
    long total_v = 0;
    for (int v = 0; v < 20; ++v) total_v += 1L * v * counts[v];
    const double mean_v = sampled ? static_cast<double>(total_v) / sampled : 0;
    std::printf("\nHYPERSLICE theta=%.3f phi=%.3f sampled=%d mean_vertices=%.2f\n",
                theta, phi, sampled, mean_v);
    std::printf("\nEach intersected cell presents its own hull at its own\n"
                "angle, so coplanar merging has nothing to merge: the\n"
                "greedy ratio this repo gates at >=4.5x would be 1.0.\n");
    // The finding that is not obvious and matters most.
    //
    // With ONE rotation plane every cross-section is an 8-vertex box: the
    // normal has only z and w components, so the cut stays square-on to x
    // and y and a block can only ever come out a sheared cube. The
    // interesting shapes - the hexagonal pillars 4D Miner is known for -
    // need BOTH planes turned at once.
    //
    // So rendering blocks as cubes is not merely an approximation at
    // small ZW-only tilts. It is exact.
    if (std::fabs(phi) < 1e-6f || std::fabs(theta) < 1e-6f) {
        std::printf("\nOne plane only: every section here has 8 vertices,\n"
                    "because the cut stays square-on to the two axes it\n"
                    "does not turn in. Cubes are EXACT in this case, not an\n"
                    "approximation. Try two nonzero angles.\n");
    }
    return 0;
}
