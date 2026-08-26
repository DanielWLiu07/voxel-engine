#include "bench/frame_report.h"

#include <glad/gl.h>

#include <chrono>
#include <cstdio>
#include <sys/resource.h>

#include "core/cpu_time.h"

namespace bench {
namespace {

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}

}  // namespace

double peak_rss_mb() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    // ru_maxrss is bytes on macOS, kilobytes on Linux.
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
}

void print_frame_report(const FrameReport& r) {
    const core::FrameStats& fs = r.stats;
    const double avg = fs.avg_ms;
    const double avg_tris = fs.frames > 0
        ? r.triangles_sum / static_cast<double>(fs.frames)
        : 0.0;
    const double tris_per_sec = avg > 0.0 ? avg_tris * 1000.0 / avg : 0.0;

    std::printf("\nBENCH_FRAME"
                " radius=%d pose=%.*s chunks=%d frames=%zu"
                " avg_ms=%.2f p50_ms=%.2f p99_ms=%.2f"
                " min_ms=%.2f max_ms=%.2f stddev_ms=%.2f avg_fps=%.1f"
                " drawn_chunks=%d drawn_sections=%d tris=%zu"
                " avg_tris=%.0f tris_per_sec=%.0f peak_rss_mb=%.1f"
                " gpu_buffers_mb=%.1f low1_fps=%.1f"
                " over_budget=%zu over_budget_pct=%.1f"
                " descheduled_frames=%zu stolen_ms=%.1f"
                " engine_low1_fps=%.1f\n",
                r.stream_radius,
                static_cast<int>(r.pose.size()), r.pose.data(),
                r.total_chunks, fs.frames,
                avg, fs.p50_ms, fs.p99_ms, fs.min_ms, fs.max_ms, fs.stddev_ms,
                (avg > 0.0 ? 1000.0 / avg : 0.0),
                r.chunks_drawn, r.sections_drawn, r.triangles_drawn,
                avg_tris, tris_per_sec, peak_rss_mb(), r.gpu_buffers_mb,
                fs.low1_fps, fs.over_budget, fs.over_budget_pct,
                fs.descheduled_frames, fs.stolen_ms, fs.engine_low1_fps);
}

void print_pass_breakdown(const PassSamples& p) {
    if (p.shadow.empty()) return;
    const double s_sh = mean(p.shadow);
    const double s_sk = mean(p.sky);
    const double s_te = mean(p.terrain);
    const double s_wa = mean(p.water);
    const double s_pf = mean(p.postfx);
    std::printf("PASS_BREAKDOWN frames=%zu"
                " shadow=%.2f sky=%.2f terrain=%.2f"
                " water=%.2f postfx=%.2f sum_passes=%.2f\n",
                p.shadow.size(),
                s_sh, s_sk, s_te, s_wa, s_pf,
                s_sh + s_sk + s_te + s_wa + s_pf);
}

}  // namespace bench

namespace bench {
namespace {

// One monotonic clock, in milliseconds, for the pass brackets. Kept local
// because nothing outside the brackets needs it.
double mono_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

FrameSampler::FrameSampler(int target_frames, bool pass_breakdown)
    : target_frames_(target_frames),
      pass_breakdown_(pass_breakdown),
      last_cpu_ms_(core::thread_cpu_ms()) {
    if (target_frames_ > 0) {
        wall_ms_.reserve(static_cast<std::size_t>(target_frames_));
        cpu_ms_.reserve(static_cast<std::size_t>(target_frames_));
    }
    if (pass_breakdown_) {
        const std::size_t n = target_frames_ > 0
            ? static_cast<std::size_t>(target_frames_) : 1024;
        passes_.shadow.reserve(n);
        passes_.sky.reserve(n);
        passes_.terrain.reserve(n);
        passes_.water.reserve(n);
        passes_.postfx.reserve(n);
    }
}

bool FrameSampler::record(double frame_ms, std::size_t triangles) {
    if (!enabled() || !settled_) return false;
    // Read every settled frame, countdown included: charging the countdown
    // to the first counted sample would put the streaming ramp's CPU time
    // into a steady-state number.
    const double cpu_now = core::thread_cpu_ms();
    const double cpu_dt  = cpu_now - last_cpu_ms_;
    last_cpu_ms_ = cpu_now;
    if (settle_remaining_ > 0) {
        --settle_remaining_;
        return false;
    }
    wall_ms_.push_back(frame_ms);
    cpu_ms_.push_back(cpu_dt);
    triangles_sum_ += static_cast<double>(triangles);
    return collected() >= target_frames_;
}

void FrameSampler::begin_pass() {
    if (!pass_breakdown_ || !settled_) return;
    // The GPU has to drain before the bracket means anything: without it
    // these would time command submission, not execution.
    glFinish();
    pass_t0_ = mono_ms();
}

void FrameSampler::end_pass(std::vector<double>& acc) {
    if (!pass_breakdown_ || !settled_) return;
    glFinish();
    acc.push_back(mono_ms() - pass_t0_);
}

core::FrameStats FrameSampler::stats() const {
    return core::compute_frame_stats(wall_ms_, cpu_ms_);
}

}  // namespace bench
