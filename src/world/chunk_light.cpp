#include "world/chunk_light.h"

#include <chrono>
#include <cstddef>
#include <vector>

namespace world {
namespace {

// BFS frontier entry, 8 bytes: int16 coordinates and a byte of level,
// rather than three ints and an int for 16. Propagation is the hot loop
// here and visits far more cells than the mesher does, so twice as many
// frontier entries per cache line is worth the casts at the push sites.
struct Cell {
    std::int16_t x, y, z;
    std::uint8_t level;
};

}  // namespace

LightStats propagate_light(const Chunk& chunk, const NeighborLight& in,
                           LightGrid& out) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    LightStats stats;
    out.clear();

    // thread_local so the worker pool does not fight over one allocation,
    // and so the capacity survives between chunks.
    thread_local std::vector<Cell> frontier;
    thread_local std::vector<Cell> next;
    frontier.clear();
    next.clear();

    auto seed = [&](int x, int y, int z, std::uint8_t level) {
        if (level == 0) return;
        if (!transmits_light(chunk.get(x, y, z))) return;
        if (out.get(x, y, z) >= level) return;
        out.set(x, y, z, level);
        frontier.push_back({static_cast<std::int16_t>(x),
                            static_cast<std::int16_t>(y),
                            static_cast<std::int16_t>(z), level});
    };

    // Emissive blocks light their own cell even though they are solid, so
    // a torch is visible rather than a dark box with a lit halo.
    for (int y = 0; y < kChunkSizeY; ++y) {
        for (int z = 0; z < kChunkSizeZ; ++z) {
            for (int x = 0; x < kChunkSizeX; ++x) {
                const std::uint8_t e = light_emission(chunk.get(x, y, z));
                if (e == 0) continue;
                ++stats.sources;
                out.set(x, y, z, e);
                frontier.push_back({static_cast<std::int16_t>(x),
                                    static_cast<std::int16_t>(y),
                                    static_cast<std::int16_t>(z), e});
            }
        }
    }

    // Light arriving from the neighbours. It has already paid one step to
    // cross the boundary, hence the -1.
    auto seed_face = [&](const LightPlane& p, bool along_z, int fixed) {
        if (!p.present) return;
        for (int y = 0; y < kChunkSizeY; ++y) {
            for (int t = 0; t < kChunkSizeX; ++t) {
                const std::uint8_t lv = p.at(t, y);
                if (lv <= 1) continue;
                if (along_z) seed(fixed, y, t, static_cast<std::uint8_t>(lv - 1));
                else         seed(t, y, fixed, static_cast<std::uint8_t>(lv - 1));
            }
        }
    };
    seed_face(in.neg_x, true, 0);
    seed_face(in.pos_x, true, kChunkSizeX - 1);
    seed_face(in.neg_z, false, 0);
    seed_face(in.pos_z, false, kChunkSizeZ - 1);

    static constexpr int kOff[6][3] = {
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}};

    while (!frontier.empty()) {
        next.clear();
        for (const Cell& c : frontier) {
            ++stats.cells_visited;
            if (c.level <= 1) continue;
            const std::uint8_t child = static_cast<std::uint8_t>(c.level - 1);
            for (const auto& o : kOff) {
                const int nx = c.x + o[0], ny = c.y + o[1], nz = c.z + o[2];
                if (nx < 0 || nx >= kChunkSizeX) continue;
                if (ny < 0 || ny >= kChunkSizeY) continue;
                if (nz < 0 || nz >= kChunkSizeZ) continue;
                if (!transmits_light(chunk.get(nx, ny, nz))) continue;
                if (out.get(nx, ny, nz) >= child) continue;
                out.set(nx, ny, nz, child);
                next.push_back({static_cast<std::int16_t>(nx),
                                static_cast<std::int16_t>(ny),
                                static_cast<std::int16_t>(nz), child});
            }
        }
        frontier.swap(next);
    }

    stats.build_ms =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    return stats;
}

}  // namespace world
