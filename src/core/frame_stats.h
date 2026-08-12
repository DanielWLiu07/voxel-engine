#pragma once

#include <cstddef>
#include <span>

namespace core {

// A 60 Hz vsync deadline. Frame cost is reported against this rather than
// as a bare frame rate because a budget is a fixed target: "3.6x inside
// budget" carries meaning to a reader who does not know what GPU produced
// it, and "215 fps" does not.
inline constexpr double kFrameBudgetMs = 1000.0 / 60.0;

// A frame counts as descheduled when it missed the deadline AND spent at
// least this share of its wall time with the render thread off-CPU. Half
// is deliberately generous: it takes an unambiguous stall, not a frame
// that merely waited on the driver for a moment, to be excused as the
// machine's fault rather than the engine's.
inline constexpr double kDescheduledOffCpuShare = 0.5;

struct FrameStats {
    std::size_t frames = 0;
    double avg_ms = 0.0;
    double p50_ms = 0.0;
    double p99_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    // A low average with a high spread still stutters, so this is the
    // consistency signal the percentiles only hint at. Sample stddev (n-1).
    double stddev_ms = 0.0;

    // The numbers a player feels, which an average hides. low1 is the mean
    // of the worst 1% of frames as fps: the rate held through the bad
    // moments, not the good ones.
    double low1_fps = 0.0;
    std::size_t over_budget = 0;
    double over_budget_pct = 0.0;

    // Frames that missed the deadline because the thread was not running,
    // and the wall time they lost off-CPU. engine_low1_fps recomputes the
    // worst 1% charging those frames only the CPU they actually used, so
    // it isolates the engine's own stutter from the machine's contention.
    // With nothing descheduled it equals low1_fps by construction, which
    // is the check that this is an attribution and not a flattering
    // recomputation.
    std::size_t descheduled_frames = 0;
    double stolen_ms = 0.0;
    double engine_low1_fps = 0.0;
};

// Per-frame wall times paired with the render thread's own CPU time for the
// same frames. `cpu_ms` may be shorter than `wall_ms` (or empty), in which
// case only the overlapping prefix is attributed and the rest is treated as
// unmeasured rather than as stall-free.
FrameStats compute_frame_stats(std::span<const double> wall_ms,
                               std::span<const double> cpu_ms);

}  // namespace core
