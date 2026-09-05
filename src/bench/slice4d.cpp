// Renders 3D slices of a 4D heightfield as PNGs, one per w.
//
// This is the question a 4D voxel engine has to answer before any of it is
// worth building: as you move along the fourth axis, does the world morph
// into a recognisably related place, or does it flicker between unrelated
// worlds? A slice that has nothing to do with its neighbour is not a
// fourth dimension, it is a seed change with extra steps.
//
// Answering it here, on a heightfield and a few hundred lines, costs a day
// instead of the weeks that answering it inside the engine would.
//
//     cmake --build build --target slice4d && ./build/slice4d
//
// Writes docs/media/slice4d/w_XX.png plus a SLICE4D line of measurements.

#include "world/noise4d.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr int kSize    = 512;   // pixels per slice, one pixel per world column
constexpr int kSlices  = 24;    // how many w values to render
float g_w_step = 0.35f;         // world units of w between slices

// The 3D generator's own constants, so a slice is directly comparable to
// the terrain the engine already makes.
constexpr int kSeaLevel  = 24;
constexpr int kSandBand  = 2;
constexpr int kStoneBand = 36;
constexpr int kSnowBand  = 40;

struct Rgb { std::uint8_t r, g, b; };

// Relief shading. Without it a heightfield drawn by colour band alone is a
// classification map: it shows which band a column is in and nothing about
// the shape of the land. The gradient is the whole point of a heightfield,
// so light it - a fixed low sun from the northwest, dotted with the
// surface normal built from the local slope.
float hillshade(int h, int h_east, int h_south) {
    const float dx = static_cast<float>(h_east - h);
    const float dz = static_cast<float>(h_south - h);
    // Normal of the surface (-dx, 1, -dz), normalized.
    const float len = std::sqrt(dx * dx + dz * dz + 1.0f);
    const float nx = -dx / len, ny = 1.0f / len, nz = -dz / len;
    // Sun low in the northwest, which rakes the slopes rather than
    // flattening them the way an overhead light would.
    constexpr float kLx = -0.55f, kLy = 0.62f, kLz = -0.55f;
    const float lambert = nx * kLx + ny * kLy + nz * kLz;
    return std::clamp(0.45f + 0.75f * lambert, 0.25f, 1.35f);
}

// The engine's surface bands, so these images read as terrain rather than
// as a heatmap. Shaded by height within each band so relief is visible.
Rgb shade(int height, float relief) {
    const float t = std::clamp(static_cast<float>(height) / 60.0f, 0.0f, 1.0f);
    // Water is flat, so relief must not darken it into mud; land takes the
    // full hillshade.
    const float lift = (height <= kSeaLevel)
        ? (0.75f + 0.35f * t)
        : (0.65f + 0.35f * t) * relief;
    auto mul = [lift](int c) {
        return static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(static_cast<float>(c) * lift), 0, 255));
    };
    if (height <= kSeaLevel)                 return {mul(40),  mul(90),  mul(160)};
    if (height <= kSeaLevel + kSandBand)     return {mul(210), mul(195), mul(140)};
    if (height >= kSnowBand)                 return {mul(240), mul(244), mul(250)};
    if (height >= kStoneBand)                return {mul(120), mul(118), mul(115)};
    return {mul(70), mul(135), mul(60)};
}

// How much noise-space a unit of player movement along w covers.
//
// This constant is the first real design finding of the 4D work, and it
// came out of running this harness rather than out of theory. With w
// treated like x and z (scale 1), stepping a block sideways in the fourth
// dimension changed 4.6% of columns by at most one block: the world was
// effectively frozen along w. The reason is not a bug. The continent field
// runs at frequency 0.004, so one noise cell is 250 units wide, and a
// player who can walk 250 units along x cannot walk 250 units along w -
// there is nothing there to walk through.
//
// So w gets its own scale. Measured with this tool over 24 slices:
//
//     scale   columns changed per step   max column jump
//       1              4.6%                    1
//       5             51.2%                    4
//      25             77.3%                    9
//      60             85.0%                   11
//
// 6 puts a single step in w at roughly half the columns moving by a block
// or two, which reads as "the world shifted" rather than as either a
// freeze or a reseed. The max jump staying in single digits across all of
// these is the important part: the field is continuous along w at every
// scale, so this is a choice about feel, not about correctness.
constexpr float kWScale = 6.0f;

// Same shape as TerrainGen::height_at: domain-warped fBm, three octave
// stacks at the 3D generator's frequencies and weights, with w threaded
// through every sample. This is the 4D analogue of the existing height
// function rather than a new design.
int height_at_4d(const world::Noise4D& warp, const world::Noise4D& continents,
                 const world::Noise4D& hills, const world::Noise4D& detail,
                 float x, float z, float w_in) {
    const float w = w_in * kWScale;
    const float ox = warp.sample(x * 0.012f, z * 0.012f, 0.0f, w * 0.012f) * 60.0f;
    const float oz = warp.sample((x + 113.0f) * 0.012f, (z + 271.0f) * 0.012f,
                                 0.0f, w * 0.012f) * 60.0f;
    const float c = continents.fbm(x + ox, z + oz, 0.0f, w, 4, 0.004f);
    const float h = hills.fbm(x, z, 0.0f, w, 4, 0.020f);
    const float d = detail.fbm(x, z, 0.0f, w, 2, 0.080f);
    const float n = c * 0.65f + h * 0.25f + d * 0.10f;
    // The 3D generator's `kSeaLevel + n * 28 + 14` does not transfer,
    // because this fbm normalizes by the amplitude sum and FastNoiseLite's
    // does not - the composite here measures p1..p99 of -0.17..0.21 rather
    // than filling [-1, 1]. Dropped into the old formula, every column
    // landed between y=31 and y=46: no water anywhere, and the whole world
    // inside the grass/stone/snow bands. The first render was a blob map
    // for exactly that reason.
    //
    // Fitted to the measured distribution instead: 115 spreads p1..p99
    // across roughly y=12..56, and the +8 offset puts about a fifth of the
    // world under sea level, so coasts exist.
    return std::clamp(
        static_cast<int>(static_cast<float>(kSeaLevel) + n * 115.0f + 8.0f),
        1, 255);
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint32_t seed = (argc > 1)
        ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 1337u;
    const std::string out_dir = (argc > 2) ? argv[2] : "docs/media/slice4d";
    if (argc > 3) g_w_step = std::strtof(argv[3], nullptr);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    // Separate seeds per field, matching the +1/+2/+3 offsets TerrainGen
    // uses, so the fields are independent rather than correlated copies.
    const world::Noise4D continents(seed), hills(seed + 1),
                         detail(seed + 2), warp(seed + 3);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSize) * kSize * 3);
    std::vector<int> prev_height;
    double worst_slice_delta = 0.0;
    double total_changed = 0.0;
    int slices_compared = 0;

    for (int s = 0; s < kSlices; ++s) {
        const float w = static_cast<float>(s) * g_w_step;
        std::vector<int> height(static_cast<std::size_t>(kSize) * kSize);

        // Heights first, colours second: the hillshade needs each column's
        // eastern and southern neighbours, so the field has to exist
        // before any of it can be shaded.
        for (int py = 0; py < kSize; ++py) {
            for (int px = 0; px < kSize; ++px) {
                // Centre the view on the origin so the same patch of world
                // is framed in every slice - the only thing changing
                // between images should be w.
                const float x = static_cast<float>(px) - kSize * 0.5f;
                const float z = static_cast<float>(py) - kSize * 0.5f;
                height[static_cast<std::size_t>(py) * kSize + px] =
                    height_at_4d(warp, continents, hills, detail, x, z, w);
            }
        }
        for (int py = 0; py < kSize; ++py) {
            for (int px = 0; px < kSize; ++px) {
                const std::size_t at = static_cast<std::size_t>(py) * kSize + px;
                const int h  = height[at];
                const int he = height[static_cast<std::size_t>(py) * kSize
                                      + std::min(px + 1, kSize - 1)];
                const int hs = height[static_cast<std::size_t>(
                                          std::min(py + 1, kSize - 1)) * kSize + px];
                const Rgb c = shade(h, hillshade(h, he, hs));
                pixels[at * 3 + 0] = c.r;
                pixels[at * 3 + 1] = c.g;
                pixels[at * 3 + 2] = c.b;
            }
        }

        char name[512];
        std::snprintf(name, sizeof name, "%s/w_%02d.png", out_dir.c_str(), s);
        if (!stbi_write_png(name, kSize, kSize, 3, pixels.data(), kSize * 3)) {
            std::fprintf(stderr, "could not write %s\n", name);
            return 1;
        }

        // The number that decides whether this is a dimension or a reseed:
        // how much the terrain moved between adjacent slices. A continuous
        // fourth axis gives small, bounded per-slice motion; unrelated
        // worlds would give changes the size of the whole height range.
        if (!prev_height.empty()) {
            double sum = 0.0;
            double worst = 0.0;
            int changed = 0;
            for (std::size_t i = 0; i < height.size(); ++i) {
                const double d = std::abs(height[i] - prev_height[i]);
                sum += d;
                worst = std::max(worst, d);
                if (d != 0.0) ++changed;
            }
            const double mean = sum / static_cast<double>(height.size());
            const double changed_pct =
                100.0 * changed / static_cast<double>(height.size());
            std::printf("  w=%5.2f  mean |dh| vs previous slice %.3f, "
                        "max %.0f, columns changed %.1f%%\n",
                        w, mean, worst, changed_pct);
            worst_slice_delta = std::max(worst_slice_delta, worst);
            total_changed += changed_pct;
            ++slices_compared;
        }
        prev_height = std::move(height);
    }

    std::printf("\nSLICE4D seed=%u slices=%d w_step=%.2f "
                "max_column_jump=%.0f mean_columns_changed_pct=%.1f\n",
                seed, kSlices, g_w_step, worst_slice_delta,
                total_changed / std::max(1, slices_compared));
    std::printf("wrote %d slices to %s\n", kSlices, out_dir.c_str());
    return 0;
}
