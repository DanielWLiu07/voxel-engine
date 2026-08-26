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

// The --bench-frame sampling harness: the settle countdown, the per-frame
// wall and CPU samples, the triangle total, and the optional glFinish-
// bracketed per-pass timers.
//
// This was eight locals and two lambdas declared together at the top of
// main and then used 700 lines apart. They are one mechanism - a state
// machine that decides when a frame counts - and three of its rules were
// only discoverable by reading every use site:
//
//   sampling starts only after the world has settled AND a further
//   settle countdown, so the samples miss the streaming ramp and the
//   post-load driver settling;
//   the CPU clock is read every frame from the moment the world settles,
//   including during the countdown, so the first counted sample is a
//   frame's worth of CPU time and not the countdown's;
//   the number of samples collected is also the orbit bench's phase,
//   which is what holds the camera at the start pose until counting
//   begins.
class FrameSampler {
public:
    FrameSampler(int target_frames, bool pass_breakdown);

    bool enabled() const { return target_frames_ > 0; }

    // The world has finished streaming. Sampling and pass timing are both
    // gated on this; set it every frame.
    void set_settled(bool settled) { settled_ = settled; }

    // Frames counted so far, which the orbit bench uses as its phase.
    int collected() const { return static_cast<int>(wall_ms_.size()); }

    // Offers one frame to the run. Returns true when the target count has
    // been reached and the report should be printed.
    bool record(double frame_ms, std::size_t triangles);

    // glFinish-bracketed pass timing. Both no-op unless pass timing was
    // asked for and the world has settled, so call sites stay unguarded.
    void begin_pass();
    void end_pass(std::vector<double>& acc);

    PassSamples& passes() { return passes_; }
    const PassSamples& passes() const { return passes_; }

    core::FrameStats stats() const;
    double triangles_sum() const { return triangles_sum_; }

private:
    // Generous (~200 ms at typical bench frame times) but cleanly clears
    // post-load shader re-jit, driver buffer-orphan settling, and the
    // cascade-warmup spike that was still surfacing in the radius-8
    // center-pose tail with a 10-frame settle.
    static constexpr int kSettleFrames = 30;

    int  target_frames_ = 0;
    bool pass_breakdown_ = false;
    bool settled_ = false;
    int  settle_remaining_ = kSettleFrames;

    std::vector<double> wall_ms_;
    std::vector<double> cpu_ms_;
    double last_cpu_ms_ = 0.0;
    double triangles_sum_ = 0.0;

    PassSamples passes_;
    double pass_t0_ = 0.0;
};

}  // namespace bench
