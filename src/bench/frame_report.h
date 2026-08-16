#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "core/frame_stats.h"

namespace bench {

// The BENCH_FRAME and PASS_BREAKDOWN lines.
//
// These are a contract, not console chatter: scripts/bench_sweep.sh and
// scripts/bench_scaling.sh parse them into the README's scaling tables. A
// contract with something outside the program should not live seventy
// lines deep inside a render loop, where the only thing holding it
// together is that nobody scrolled past it.
//
// Nothing here touches GL. The caller reads the counters off the renderer
// and hands over numbers, which is also what keeps the platform-specific
// peak-RSS lookup out of main.cpp.

struct FrameReport {
    int stream_radius = 0;
    std::string_view pose;
    int total_chunks = 0;

    core::FrameStats stats;

    // Summed over the sampled frames, not read off the last one: a moving
    // camera draws a different count each frame, so throughput has to
    // divide total triangles by total time rather than multiply the
    // average frame time by one arbitrary frame's count.
    double triangles_sum = 0.0;

    int chunks_drawn = 0;
    int sections_drawn = 0;
    std::size_t triangles_drawn = 0;

    // Vertex + index bytes resident across all chunks: the VRAM analogue
    // of the peak RSS this fills in for itself.
    double gpu_buffers_mb = 0.0;
};

// Peak resident set size of this process, in MB. Wraps the one place the
// engine cares that ru_maxrss is bytes on macOS and kilobytes on Linux.
double peak_rss_mb();

void print_frame_report(const FrameReport& r);

// Per-pass GPU timings, each a per-frame sample. Prints nothing when the
// pass timers were not enabled.
struct PassSamples {
    std::vector<double> shadow;
    std::vector<double> sky;
    std::vector<double> terrain;
    std::vector<double> water;
    std::vector<double> postfx;
};

void print_pass_breakdown(const PassSamples& p);

}  // namespace bench
