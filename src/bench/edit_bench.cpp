#include "bench/edit_bench.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace bench {

ModeResult run_edit_bench(world::World& wrld, int edits, int stream_radius) {
        std::vector<double> edit_samples;
        edit_samples.reserve(static_cast<std::size_t>(edits));
        int probe = 0;
        // Positions walk a deterministic lattice over a 7x7-chunk
        // neighborhood at underground depths that are solid on any
        // seed's terrain, so break edits never no-op.
        while (static_cast<int>(edit_samples.size()) < edits &&
               probe < edits * 64) {
            const int x = ((probe * 37) % 112) - 56;
            const int z = ((probe * 53) % 112) - 56;
            const int y = 20 + (probe % 30);
            ++probe;
            const world::BlockId prev = wrld.block_at(x, y, z);
            if (prev == world::BlockId::Air) continue;
            const auto t0 = std::chrono::steady_clock::now();
            wrld.set_block(x, y, z, world::BlockId::Air);
            edit_samples.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count());
            if (static_cast<int>(edit_samples.size()) < edits) {
                const auto t1 = std::chrono::steady_clock::now();
                wrld.set_block(x, y, z, prev);
                edit_samples.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t1).count());
            } else {
                wrld.set_block(x, y, z, prev);  // restore untimed
            }
        }
        if (edit_samples.empty()) {
            std::fprintf(stderr, "--bench-edit found no solid blocks to edit\n");
            return ModeResult::Failed;
        }
        std::vector<double> sorted = edit_samples;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t n = sorted.size();
        double sum = 0.0;
        for (double s : sorted) sum += s;
        const double p50 = sorted[n / 2];
        const double p99 = sorted[std::min<std::size_t>(n - 1,
            static_cast<std::size_t>(static_cast<double>(n) * 0.99))];
        std::printf("\nBENCH_EDIT edits=%zu avg_ms=%.3f p50_ms=%.3f "
                    "p99_ms=%.3f max_ms=%.3f radius=%d\n",
                    n, sum / static_cast<double>(n), p50, p99,
                    sorted[n - 1], stream_radius);
        return ModeResult::Finished;
}

}  // namespace bench
