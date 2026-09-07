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
#include "world/chunk_mesh.h"
#include "world/hyperslice.h"
#include "world/prism_mesh.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
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

// Draws the cell tiling itself, so the shapes can be looked at rather
// than counted. Each pixel is coloured by which 4D lattice cell the slice
// point belongs to, so the boundaries between cells ARE the block edges a
// player would see looking straight down.
int write_map(float theta, float phi, const char* path) {
    constexpr int kPx = 900;        // pixels
    constexpr float kSpan = 18.0f;  // blocks across
    world::TerrainGen4D::Slice s{};
    s.theta = theta;
    s.phi = phi;
    const auto b = world::SliceBasis::from(s);

    std::vector<unsigned char> img(static_cast<std::size_t>(kPx) * kPx * 3);
    for (int py = 0; py < kPx; ++py) {
        for (int px = 0; px < kPx; ++px) {
            const float sx = (static_cast<float>(px) / kPx) * kSpan;
            const float sz = (static_cast<float>(py) / kPx) * kSpan;
            const int i = static_cast<int>(std::floor(
                b.x4_sx * sx + b.x4_sz * sz + b.x4_c));
            const int k = static_cast<int>(std::floor(
                b.z4_sx * sx + b.z4_sz * sz + b.z4_c));
            const int l = static_cast<int>(std::floor(
                b.w4_sx * sx + b.w4_sz * sz + b.w4_c));
            // A stable colour per cell, so neighbouring cells differ and
            // the tiling is visible as shapes rather than as a gradient.
            std::uint32_t h = static_cast<std::uint32_t>(i * 73856093)
                            ^ static_cast<std::uint32_t>(k * 19349663)
                            ^ static_cast<std::uint32_t>(l * 83492791);
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            const std::size_t at = (static_cast<std::size_t>(py) * kPx + px) * 3;
            img[at + 0] = static_cast<unsigned char>(110 + (h & 0x7f));
            img[at + 1] = static_cast<unsigned char>(110 + ((h >> 8) & 0x7f));
            img[at + 2] = static_cast<unsigned char>(110 + ((h >> 16) & 0x7f));
        }
    }
    if (!stbi_write_png(path, kPx, kPx, 3, img.data(), kPx * 3)) {
        std::fprintf(stderr, "could not write %s\n", path);
        return 1;
    }
    std::printf("wrote %s (%d px, %.0f blocks across, theta=%.2f phi=%.2f)\n",
                path, kPx, kSpan, theta, phi);
    return 0;
}

// What a chunk costs meshed as 4D cross-sections, against the greedy cube
// mesher on the same terrain.
//
// Reported as ratios of COUNTS first and timings second, and in that
// order deliberately: a quad count is a property of the geometry and
// reproduces on any machine, while a millisecond is a property of this
// one under whatever else it was doing. The repo has been burned by
// quoting the second kind as though it were the first.
int mesh_cost(int side) {
    struct Cfg { const char* name; float theta, phi; };
    const Cfg cfgs[] = {
        {"flat",      0.00f, 0.00f},
        {"zw only",   0.40f, 0.00f},
        {"both",      0.40f, 0.40f},
        {"hard tilt", 0.90f, 0.70f},
    };

    world::TerrainGen4D gen(1337);
    std::printf("prism mesh cost against the greedy cube mesher, "
                "%dx%d chunks, seed 1337\n\n", side, side);
    std::printf("  %-10s  %8s  %10s  %10s  %7s  %9s  %9s\n",
                "cut", "cells", "prism qd", "greedy qd", "ratio",
                "prism ms", "greedy ms");

    const int lo = -(side / 2);
    const int hi = lo + side - 1;
    for (const Cfg& cfg : cfgs) {
        world::TerrainGen4D::Slice s{};
        s.theta = cfg.theta;
        s.phi   = cfg.phi;

        long cells = 0, prism_quads = 0, greedy_quads = 0;
        double prism_ms = 0.0, greedy_ms = 0.0;
        int chunks = 0;
        for (int cx = lo; cx <= hi; ++cx) {
            for (int cz = lo; cz <= hi; ++cz) {
                const auto t0 = std::chrono::steady_clock::now();
                world::PrismChunk pc = world::build_prism_chunk(gen, {cx, cz}, s);
                const auto pm = world::build_prism_mesh(pc);
                prism_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                cells += static_cast<long>(pc.cells.size());
                prism_quads += pm.quad_count;

                world::Chunk cube;
                const auto t1 = std::chrono::steady_clock::now();
                gen.fill_chunk(cx, cz, s, cube);
                const auto cm = world::build_chunk_mesh_greedy(cube);
                greedy_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t1).count();
                greedy_quads += cm.quad_count;
                ++chunks;
            }
        }
        std::printf("  %-10s  %8.0f  %10ld  %10ld  %6.2fx  %8.2f  %9.2f\n",
                    cfg.name, static_cast<double>(cells) / chunks,
                    prism_quads, greedy_quads,
                    static_cast<double>(prism_quads) / static_cast<double>(greedy_quads),
                    prism_ms / chunks, greedy_ms / chunks);
    }
    std::printf("\nCells per chunk is 256 at a flat cut, which is the voxel\n"
                "grid exactly. It rises with tilt because a turned\n"
                "hyperplane passes through more cells per unit of area.\n"
                "The quad ratio is the honest cost of the look: caps cannot\n"
                "merge, because neighbouring cells present their own\n"
                "polygons at their own angles.\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && (std::string(argv[1]) == "--help" ||
                     std::string(argv[1]) == "-h")) {
        std::printf(
            "hyperslice - what shape a 4D block presents to a tilted cut\n\n"
            "  hyperslice [theta] [phi]\n"
            "      sweep a hyperplane through one tesseract and report the\n"
            "      cross-sections it produces (default 0.15 0.0)\n\n"
            "  hyperslice --map [theta] [phi] [out.png]\n"
            "      draw the cell tiling looking straight down, coloured by\n"
            "      cell, so the block shapes can be seen rather than counted\n"
            "      (default docs/media/cells.png)\n\n"
            "  hyperslice --mesh [chunks]\n"
            "      what a chunk costs to mesh as prisms against the greedy\n"
            "      cube mesher on the same terrain, over a grid of chunks\n"
            "      (default 5, i.e. 25 chunks)\n\n"
            "  hyperslice --help\n");
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--mesh") {
        const int side = (argc > 2) ? std::atoi(argv[2]) : 5;
        return mesh_cost(std::max(1, side));
    }
    if (argc > 1 && std::string(argv[1]) == "--map") {
        const float th = (argc > 2) ? std::strtof(argv[2], nullptr) : 0.4f;
        const float ph = (argc > 3) ? std::strtof(argv[3], nullptr) : 0.4f;
        const char* out = (argc > 4) ? argv[4] : "docs/media/cells.png";
        return write_map(th, ph, out);
    }

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
