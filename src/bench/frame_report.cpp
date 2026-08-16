#include "bench/frame_report.h"

#include <cstdio>
#include <sys/resource.h>

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
