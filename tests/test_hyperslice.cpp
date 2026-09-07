// The shape a 4D block presents to a tilted slice.
//
// The engine's normal path draws blocks as cubes. These checks pin when
// that is exact and when it is not, because the answer is not obvious and
// I had it wrong: with the cut turned in ONE plane a block really is a
// box, and only a compound turn produces the hexagonal pillars a 4D
// slicer is known for.

#include "world/hyperslice.h"

#include <cmath>
#include <cstdio>
#include <set>

namespace {

int failures = 0;
int checks = 0;
#define EXPECT(cond, what)                                                    \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) {                                                        \
            ++failures;                                                       \
            std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, what);     \
        }                                                                     \
    } while (0)

// Every distinct vertex count a cell presents across a patch of the slice.
std::set<int> shapes_over_patch(world::TerrainGen4D::Slice s) {
    const auto b = world::SliceBasis::from(s);
    std::set<int> seen;
    for (int i = -6; i <= 6; ++i)
        for (int k = -6; k <= 6; ++k)
            for (int l = -6; l <= 6; ++l) {
                const auto p = world::cell_polygon(i, k, l, b, -8.0f, -8.0f, 16.0f);
                if (!p.empty()) seen.insert(p.count);
            }
    return seen;
}

void test_an_untilted_cut_presents_squares() {
    // theta = phi = 0: the slice IS a lattice hyperplane, so a cell's
    // preimage is the unit square. Blocks are cubes, exactly.
    world::TerrainGen4D::Slice s{};
    const auto shapes = shapes_over_patch(s);
    EXPECT(shapes.size() == 1, "an axis-aligned cut presents one shape");
    EXPECT(shapes.count(4) == 1, "and that shape is a square");
}

void test_one_plane_still_presents_boxes() {
    // The finding that corrected me. Turning only in ZW leaves the cut
    // square-on to x, so the polygon keeps four sides however far it
    // turns - drawing these blocks as cubes is exact, not an
    // approximation.
    for (const float theta : {0.15f, 0.4f, 0.8f}) {
        world::TerrainGen4D::Slice s{};
        s.theta = theta;
        const auto shapes = shapes_over_patch(s);
        EXPECT(shapes.count(5) == 0 && shapes.count(6) == 0,
               "a single-plane cut presents no five- or six-sided cells");
    }
}

void test_a_compound_cut_presents_hexagons() {
    // Both planes at once, which is what the wheel alone cannot reach.
    world::TerrainGen4D::Slice s{};
    s.theta = 0.4f;
    s.phi   = 0.4f;
    const auto shapes = shapes_over_patch(s);
    EXPECT(shapes.count(5) + shapes.count(6) > 0,
           "a compound cut presents five- and six-sided cells");
    EXPECT(*shapes.rbegin() <= 6,
           "and never more than six: six half-planes bound six sides");
}

void test_a_cell_far_from_the_cut_presents_nothing() {
    world::TerrainGen4D::Slice s{};
    s.theta = 0.3f;
    const auto b = world::SliceBasis::from(s);
    const auto p = world::cell_polygon(400, 400, 400, b, -8.0f, -8.0f, 16.0f);
    EXPECT(p.empty(), "a distant cell does not intersect the patch");
}

void test_the_cells_tile_the_patch_without_gaps_or_overlap() {
    // The strongest property here, and the one a renderer depends on:
    // every point of the slice belongs to exactly one cell, so the
    // polygons partition the patch. Their areas must sum to its area.
    //
    // Without this a mesher would either leave holes in the world or draw
    // the same ground twice, and both look like a bug in something else.
    world::TerrainGen4D::Slice s{};
    s.theta = 0.35f;
    s.phi   = 0.25f;
    const auto b = world::SliceBasis::from(s);
    double area = 0.0;
    constexpr float kSpan = 8.0f;
    for (int i = -12; i <= 12; ++i)
        for (int k = -12; k <= 12; ++k)
            for (int l = -12; l <= 12; ++l) {
                const auto p = world::cell_polygon(i, k, l, b, 0.0f, 0.0f, kSpan);
                if (p.empty()) continue;
                double a = 0.0;
                for (int v = 0; v < p.count; ++v) {
                    const int n = (v + 1) % p.count;
                    a += static_cast<double>(p.x[v]) * p.z[n]
                       - static_cast<double>(p.x[n]) * p.z[v];
                }
                area += std::fabs(a) * 0.5;
            }
    const double want = static_cast<double>(kSpan) * kSpan;
    EXPECT(std::fabs(area - want) < 0.01,
           "the cell polygons tile the patch exactly");
}

}  // namespace

int main() {
    test_an_untilted_cut_presents_squares();
    test_one_plane_still_presents_boxes();
    test_a_compound_cut_presents_hexagons();
    test_a_cell_far_from_the_cut_presents_nothing();
    test_the_cells_tile_the_patch_without_gaps_or_overlap();
    std::printf("\nhyperslice_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
