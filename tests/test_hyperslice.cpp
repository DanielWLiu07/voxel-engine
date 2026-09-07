// The shape a 4D block presents to a tilted slice.
//
// The engine's normal path draws blocks as cubes. These checks pin when
// that is exact and when it is not, because the answer is not obvious and
// I had it wrong: with the cut turned in ONE plane a block really is a
// box, and only a compound turn produces the hexagonal pillars a 4D
// slicer is known for.

#include "world/hyperslice.h"

#include <array>
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


void test_enumeration_finds_every_cell_the_hand_search_does() {
    // for_each_cell exists so a mesher does not have to guess a search
    // box. It must find exactly what the exhaustive scan above finds -
    // no more (wasted clips are free but a duplicate would draw ground
    // twice) and above all no fewer, since a missed cell is a hole.
    for (const auto s : {world::TerrainGen4D::Slice{},
                         world::TerrainGen4D::Slice{0.0f, 0.5f, 0.0f, 0.0f, 0.0f},
                         world::TerrainGen4D::Slice{1.5f, 0.35f, 0.0f, 0.25f, 0.0f},
                         world::TerrainGen4D::Slice{-3.0f, -0.7f, 4.0f, 0.9f, -2.0f}}) {
        const auto b = world::SliceBasis::from(s);
        constexpr float kX0 = -3.0f, kZ0 = 5.0f, kSpan = 16.0f;

        std::set<std::array<int, 3>> want;
        for (int i = -60; i <= 60; ++i)
            for (int k = -60; k <= 60; ++k)
                for (int l = -60; l <= 60; ++l)
                    if (!world::cell_polygon(i, k, l, b, kX0, kZ0, kSpan).empty())
                        want.insert({i, k, l});

        std::set<std::array<int, 3>> got;
        int visits = 0;
        world::for_each_cell(b, kX0, kZ0, kSpan,
                             [&](int i, int k, int l, const world::SlicePolygon&) {
                                 got.insert({i, k, l});
                                 ++visits;
                             });
        EXPECT(!want.empty(), "the hand search found something to compare against");
        EXPECT(got == want, "for_each_cell finds exactly the cells that meet the patch");
        EXPECT(visits == static_cast<int>(got.size()),
               "and visits each of them once");
    }
}

void test_an_untilted_patch_enumerates_one_cell_per_column() {
    // The floor under every cost claim: with the cut axis-aligned the
    // tiling IS the voxel grid, so 4D mode pays nothing for a world it is
    // not tilting.
    world::TerrainGen4D::Slice s{};
    const auto b = world::SliceBasis::from(s);
    int cells = 0;
    world::for_each_cell(b, 0.0f, 0.0f, 16.0f,
                         [&](int, int, int, const world::SlicePolygon& p) {
                             if (world::polygon_area(p) > 1e-4f) ++cells;
                         });
    EXPECT(cells == 256, "a flat 16x16 patch tiles into exactly 256 cells");
}

void test_enumerated_polygons_tile_the_patch() {
    // The same partition property as above, but through the entry point
    // the mesher actually calls, and at a patch offset that is not the
    // origin - an off-origin patch is where a sign error in the search
    // box would show up.
    world::TerrainGen4D::Slice s{2.5f, 0.35f, 1.0f, 0.25f, -2.0f};
    const auto b = world::SliceBasis::from(s);
    constexpr float kSpan = 16.0f;
    double area = 0.0;
    world::for_each_cell(b, 64.0f, -48.0f, kSpan,
                         [&](int, int, int, const world::SlicePolygon& p) {
                             area += std::fabs(world::polygon_area(p));
                         });
    EXPECT(std::fabs(area - kSpan * kSpan) < 0.02,
           "the enumerated polygons tile the patch exactly");
}


void test_walking_on_a_tilted_cut_is_travel_along_w() {
    // The property behind "when I walk around, am I moving in 4D?".
    //
    // On a FLAT cut, no: the slice is the hyperplane w = constant, so
    // every point of it sits at the same w and walking only reveals more
    // of the same cross-section. On a TILTED cut, yes and unavoidably so:
    // the slice's own z axis leans into w, so a step forward is a step
    // along the fourth axis whether the player asked for one or not.
    //
    // This is not a feature that could be switched off. It is what a
    // tilted hyperplane IS, and it is why the tilt is the control that
    // makes the world feel four-dimensional rather than the travel keys.
    struct Case { float theta, phi; int max_blocks; };
    const Case cases[] = {
        {0.00f, 0.00f, 0},      // flat: never
        {0.15f, 0.00f, 8},      // one notch of wheel: a cell every ~7 blocks
        {0.45f, 0.45f, 3},      // both planes: every ~2.3
        {0.90f, 0.70f, 2},      // hard tilt: almost every block
    };
    for (const Case& c : cases) {
        world::TerrainGen4D::Slice s{};
        s.theta = c.theta;
        s.phi   = c.phi;
        const auto b = world::SliceBasis::from(s);

        auto w_cell_at = [&](float sx, float sz) {
            return static_cast<int>(std::floor(
                b.w4_sx * sx + b.w4_sz * sz + b.w4_c));
        };
        const int start = w_cell_at(0.5f, 0.5f);

        if (c.max_blocks == 0) {
            // Flat: walk the whole chunk in both directions and never
            // leave the w cell you started in.
            bool moved = false;
            for (int t = 0; t < 16; ++t) {
                if (w_cell_at(static_cast<float>(t) + 0.5f, 0.5f) != start) moved = true;
                if (w_cell_at(0.5f, static_cast<float>(t) + 0.5f) != start) moved = true;
            }
            EXPECT(!moved, "walking a flat cut never moves you along w");
            continue;
        }

        // Tilted: crossing into a different w cell must happen within a
        // few blocks of walking. Bounding it from ABOVE is the point -
        // "eventually" would pass on a cut so nearly flat that nothing
        // the player does reads as four-dimensional.
        int crossed_at = -1;
        for (int t = 1; t <= 32 && crossed_at < 0; ++t) {
            if (w_cell_at(0.5f, static_cast<float>(t) + 0.5f) != start) crossed_at = t;
        }
        EXPECT(crossed_at > 0, "walking a tilted cut moves you along w");
        EXPECT(crossed_at > 0 && crossed_at <= c.max_blocks,
               "and it does so within a few blocks, not eventually");
    }
}


void test_a_box_of_cells_presents_a_convex_polygon() {
    // The result the prism mesher's merge rests on, and the one its own
    // comments used to deny: a lattice BOX presents a convex polygon to
    // the slice, at any angle and however many cells it spans. The
    // preimage of a convex set under a linear map is convex; to_4d is
    // linear; a box is convex. That is the whole proof, and this is it
    // measured.
    //
    // If it failed, greedy merging on a tilted cut would produce
    // non-convex faces and the fan triangulation would fold them inside
    // out - which is exactly the shape of bug that reads as random holes.
    int checked = 0, nonconvex = 0, too_many_sides = 0;
    for (const auto s : {world::TerrainGen4D::Slice{},
                         world::TerrainGen4D::Slice{0.0f, 0.45f, 0.0f, 0.45f, 0.0f},
                         world::TerrainGen4D::Slice{2.0f, 0.9f, 3.0f, -0.7f, -1.0f}}) {
        const auto b = world::SliceBasis::from(s);
        for (int i0 = -3; i0 <= 3; ++i0)
            for (int span_i = 0; span_i <= 4; ++span_i)
                for (int k0 = -3; k0 <= 3; ++k0)
                    for (int span_k = 0; span_k <= 4; ++span_k)
                        for (int l = -3; l <= 3; ++l) {
                            const auto p = world::box_polygon(
                                i0, i0 + span_i, k0, k0 + span_k, l, b,
                                0.0f, 0.0f, 16.0f);
                            if (p.empty()) continue;
                            ++checked;
                            if (p.count > world::SlicePolygon::kMaxSides)
                                ++too_many_sides;
                            // Convex: every cross product of consecutive
                            // edges has the same sign.
                            int pos = 0, neg = 0;
                            for (int v = 0; v < p.count; ++v) {
                                const int a = (v + 1) % p.count;
                                const int c = (v + 2) % p.count;
                                const float ux = p.x[a] - p.x[v], uz = p.z[a] - p.z[v];
                                const float vx = p.x[c] - p.x[a], vz = p.z[c] - p.z[a];
                                const float cross = ux * vz - uz * vx;
                                if (cross >  1e-5f) ++pos;
                                if (cross < -1e-5f) ++neg;
                            }
                            if (pos > 0 && neg > 0) ++nonconvex;
                        }
    }
    EXPECT(checked > 2000, "the sweep found boxes that meet the patch");
    EXPECT(nonconvex == 0, "a box of cells always presents a convex polygon");
    EXPECT(too_many_sides == 0,
           "and never more than ten sides - six lattice half-planes plus "
           "the four of the patch it is clipped to");
}

void test_a_one_cell_box_is_the_cell() {
    // box_polygon generalises cell_polygon, so the degenerate box has to
    // agree with it - otherwise the merged path and the unmerged path
    // draw different worlds wherever a run happens to be length one.
    //
    // Compared before ownership, because that is the one thing
    // cell_polygon adds: box_polygon has no half-open tie to break, since
    // a mesher only ever asks it about boxes it already owns.
    world::TerrainGen4D::Slice s{1.0f, 0.35f, 0.0f, 0.25f, 0.0f};
    const auto b = world::SliceBasis::from(s);
    int compared = 0, differing = 0;
    for (int i = -8; i <= 8; ++i)
        for (int k = -8; k <= 8; ++k)
            for (int l = -8; l <= 8; ++l) {
                const auto cell = world::cell_polygon(i, k, l, b, 0.0f, 0.0f, 16.0f);
                if (cell.empty()) continue;
                const auto box = world::box_polygon(i, i, k, k, l, b, 0.0f, 0.0f, 16.0f);
                ++compared;
                if (box.count != cell.count) { ++differing; continue; }
                for (int v = 0; v < cell.count; ++v) {
                    if (std::fabs(box.x[v] - cell.x[v]) > 1e-4f ||
                        std::fabs(box.z[v] - cell.z[v]) > 1e-4f) {
                        ++differing;
                        break;
                    }
                }
            }
    EXPECT(compared > 100, "there were single cells to compare");
    EXPECT(differing == 0, "a one-cell box is exactly that cell's polygon");
}

}  // namespace

int main() {
    test_an_untilted_cut_presents_squares();
    test_one_plane_still_presents_boxes();
    test_a_compound_cut_presents_hexagons();
    test_a_cell_far_from_the_cut_presents_nothing();
    test_the_cells_tile_the_patch_without_gaps_or_overlap();
    test_a_box_of_cells_presents_a_convex_polygon();
    test_a_one_cell_box_is_the_cell();
    test_walking_on_a_tilted_cut_is_travel_along_w();
    test_enumeration_finds_every_cell_the_hand_search_does();
    test_an_untilted_patch_enumerates_one_cell_per_column();
    test_enumerated_polygons_tile_the_patch();
    std::printf("\nhyperslice_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
