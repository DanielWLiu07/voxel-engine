// What does the fourth dimension cost?
//
// A 4D voxel world reaches a 3D renderer by slicing, so moving along w
// swaps the whole visible slice. Every chunk's contents change, which
// means every chunk's mesh is invalid, which means a step along w is a
// full world re-mesh. That is the central engineering problem of a 4D
// voxel engine, and as far as I can find nobody has published what it
// costs - games that ship 4D worlds do not report it.
//
// This measures it, three ways:
//
//   1. The naive cost: regenerate and re-mesh every chunk in the window.
//   2. How much of that is avoidable, by asking how many chunks actually
//      differ between adjacent slices. A chunk whose contents are
//      identical at w and w+1 does not need re-meshing at all.
//   3. What that leaves, as a projected step time.
//
//     cmake --build build --target wcost && ./build/wcost [radius] [seed]
//
// CPU only. No GL, no threads - a single-threaded number that divides
// cleanly by a worker count rather than one entangled with a scheduler.

#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/terrain_gen4d.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

double ms_since(clock_type::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
}

// Content fingerprints at three granularities, because which one matters
// is the whole question. FNV-1a; no realistic collision risk over 64 KB
// and far faster than comparing arrays pairwise across a window.
//
// Chunk granularity is what a re-mesh currently costs, since the mesher
// builds a chunk-wide mesh and then buckets it into sections. Section
// granularity is what the renderer culls and uploads, so it is what a
// redesigned mesher could plausibly rebuild independently. Column
// granularity is the floor: the least that could possibly need touching.
struct Prints {
    std::uint64_t chunk = 0;
    std::array<std::uint64_t, world::kSectionsPerChunk> sections{};
    std::array<std::uint64_t, world::kChunkSizeX * world::kChunkSizeZ> columns{};
};

Prints fingerprint(const world::Chunk& c) {
    constexpr std::uint64_t kBasis = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    Prints p;
    p.chunk = kBasis;
    p.sections.fill(kBasis);
    p.columns.fill(kBasis);
    for (int y = 0; y < world::kChunkSizeY; ++y) {
        const int section = y / world::kSectionHeight;
        for (int z = 0; z < world::kChunkSizeZ; ++z) {
            for (int x = 0; x < world::kChunkSizeX; ++x) {
                const auto b = static_cast<std::uint8_t>(c.get(x, y, z));
                p.chunk = (p.chunk ^ b) * kPrime;
                p.sections[static_cast<std::size_t>(section)] =
                    (p.sections[static_cast<std::size_t>(section)] ^ b) * kPrime;
                const std::size_t col =
                    static_cast<std::size_t>(z) * world::kChunkSizeX + x;
                p.columns[col] = (p.columns[col] ^ b) * kPrime;
            }
        }
    }
    return p;
}

struct SliceCost {
    double gen_ms = 0.0;
    double mesh_ms = 0.0;
    std::size_t quads = 0;
};

SliceCost build_slice(const world::TerrainGen4D& terrain, int radius, int w,
                      std::vector<world::Chunk>& out,
                      std::vector<Prints>& prints) {
    SliceCost cost;
    const int side = 2 * radius + 1;
    out.assign(static_cast<std::size_t>(side) * side, world::Chunk{});
    prints.assign(out.size(), Prints{});

    // Generate the whole slice first, so every chunk has its neighbours
    // available when it is meshed.
    std::size_t i = 0;
    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx, ++i) {
            const auto t0 = clock_type::now();
            terrain.fill_chunk(cx, cz, w, out[i]);
            cost.gen_ms += ms_since(t0);
            prints[i] = fingerprint(out[i]);
        }
    }

    // Meshed against neighbours, not in isolation.
    //
    // This used to pass {} for NeighborPlanes, which is the
    // pre-cross-chunk-culling configuration the engine stopped using -
    // and which the repo's headline memory figure exists because it
    // abandoned. It reported 108,254 quads at radius 6 against the 92,136
    // the shipped path produces, 17.5% too many. Mesh time was only 1.4%
    // higher and the 1.70x conclusion did not move, but a quad count from
    // a configuration nothing ships is the kind of number that gets
    // quoted later and is wrong when it is.
    auto chunk_at = [&](int cx, int cz) -> const world::Chunk* {
        if (cx < -radius || cx > radius || cz < -radius || cz > radius) {
            return nullptr;  // outside the window: genuinely unknown
        }
        const std::size_t idx = static_cast<std::size_t>(cz + radius) * side
                              + static_cast<std::size_t>(cx + radius);
        return &out[idx];
    };
    i = 0;
    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx, ++i) {
            const world::NeighborChunks n{
                chunk_at(cx - 1, cz), chunk_at(cx + 1, cz),
                chunk_at(cx, cz - 1), chunk_at(cx, cz + 1)};
            const auto planes = world::NeighborPlanes::from(n);
            const auto t1 = clock_type::now();
            const auto mesh = world::build_chunk_mesh(
                world::MesherKind::Greedy, out[i], planes, {});
            cost.mesh_ms += ms_since(t1);
            cost.quads += static_cast<std::size_t>(mesh.quad_count);
        }
    }
    return cost;
}

}  // namespace

int main(int argc, char** argv) {
    const int radius = (argc > 1) ? std::atoi(argv[1]) : 6;
    const std::uint32_t seed = (argc > 2)
        ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 1337u;
    if (radius < 1 || radius > 24) {
        std::fprintf(stderr, "radius must be 1..24\n");
        return 1;
    }

    const world::TerrainGen4D terrain(seed);
    const int side = 2 * radius + 1;
    const int chunks = side * side;

    std::vector<world::Chunk> slice_a, slice_b;
    std::vector<Prints> print_a, print_b;

    std::printf("4D w-traversal cost, radius %d (%d chunks), seed %u\n\n",
                radius, chunks, seed);

    const SliceCost first = build_slice(terrain, radius, 0, slice_a, print_a);
    std::printf("  slice w=0 built: gen %.1f ms, mesh %.1f ms, %zu quads\n",
                first.gen_ms, first.mesh_ms, first.quads);

    // Step along w and measure both the full rebuild and how much of it
    // was actually necessary.
    constexpr int kSteps = 4;
    double total_step_ms = 0.0;
    double total_gen_ms = 0.0, total_mesh_ms = 0.0;
    std::size_t total_changed = 0;
    std::size_t total_chunks = 0;
    std::size_t total_sections = 0, total_sections_all = 0;
    std::size_t total_columns = 0, total_columns_all = 0;
    for (int step = 1; step <= kSteps; ++step) {
        const SliceCost c = build_slice(terrain, radius, step, slice_b, print_b);
        const double step_ms = c.gen_ms + c.mesh_ms;

        std::size_t changed = 0, changed_sections = 0, changed_columns = 0;
        for (std::size_t i = 0; i < print_a.size(); ++i) {
            if (print_a[i].chunk != print_b[i].chunk) ++changed;
            for (std::size_t s2 = 0; s2 < print_a[i].sections.size(); ++s2) {
                if (print_a[i].sections[s2] != print_b[i].sections[s2]) ++changed_sections;
            }
            for (std::size_t col = 0; col < print_a[i].columns.size(); ++col) {
                if (print_a[i].columns[col] != print_b[i].columns[col]) ++changed_columns;
            }
        }
        total_step_ms += step_ms;
        total_gen_ms += c.gen_ms;
        total_mesh_ms += c.mesh_ms;
        total_changed += changed;
        total_chunks += print_a.size();
        total_sections += changed_sections;
        total_sections_all += print_a.size() * world::kSectionsPerChunk;
        total_columns += changed_columns;
        total_columns_all +=
            print_a.size() * world::kChunkSizeX * world::kChunkSizeZ;

        std::printf("  w=%d -> w=%d: rebuild %.1f ms (gen %.1f + mesh %.1f) | "
                    "changed: %.1f%% chunks, %.1f%% sections, %.1f%% columns\n",
                    step - 1, step, step_ms, c.gen_ms, c.mesh_ms,
                    100.0 * static_cast<double>(changed) / chunks,
                    100.0 * static_cast<double>(changed_sections)
                        / (chunks * world::kSectionsPerChunk),
                    100.0 * static_cast<double>(changed_columns)
                        / (chunks * world::kChunkSizeX * world::kChunkSizeZ));
        print_a.swap(print_b);
    }

    const double mean_step_ms = total_step_ms / kSteps;
    const double chunk_frac =
        static_cast<double>(total_changed) / static_cast<double>(total_chunks);
    const double section_frac = static_cast<double>(total_sections)
        / static_cast<double>(total_sections_all);
    const double column_frac = static_cast<double>(total_columns)
        / static_cast<double>(total_columns_all);

    // Generation is unavoidable: you cannot know a chunk is unchanged
    // without generating it. Meshing is the half an incremental scheme
    // skips, and at these ratios it is also the larger half.
    //
    // Split using the steps' own totals rather than the first slice's
    // ratio. Slice 0 is measurably cold - it ran gen 103 / mesh 118
    // against the steps' 94 / 103 - so deriving the share from it skewed
    // the split by about 2 ms while the per-step numbers were already
    // being computed and thrown away.
    const double mean_gen_ms = total_gen_ms / kSteps;
    const double mean_mesh_ms = total_mesh_ms / kSteps;

    std::printf("\nWCOST radius=%d chunks=%d full_step_ms=%.1f "
                "chunk_frac=%.3f section_frac=%.3f column_frac=%.3f\n",
                radius, chunks, mean_step_ms,
                chunk_frac, section_frac, column_frac);

    // What each granularity would buy, if a mesher could rebuild at it.
    // Chunk granularity is what today's mesher works at; the others are
    // what a redesign would have to reach to be worth doing.
    std::printf("\nif re-meshing could skip unchanged units:\n");
    struct Row { const char* name; double frac; };
    const Row rows[] = {{"chunk   (today's mesher)", chunk_frac},
                        {"section (renderer's unit)", section_frac},
                        {"column  (theoretical floor)", column_frac}};
    for (const Row& r : rows) {
        const double ms = mean_gen_ms + mean_mesh_ms * r.frac;
        std::printf("  %-28s %5.1f ms/step  (%.2fx)  %5.0f ms on 9 workers\n",
                    r.name, ms, ms > 0.0 ? mean_step_ms / ms : 0.0, ms / 9.0);
    }
    std::printf("\nsingle-threaded numbers; the 9-worker column assumes the "
                "engine's pool size\n");
    return 0;
}
