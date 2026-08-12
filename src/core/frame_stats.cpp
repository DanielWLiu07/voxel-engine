#include "core/frame_stats.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace core {

namespace {

// True when this frame missed the deadline because the thread was not on a
// core, rather than because the engine had too much to do.
bool descheduled(double wall, double cpu) {
    return wall > kFrameBudgetMs &&
           (wall - cpu) > wall * kDescheduledOffCpuShare;
}

// Mean of the worst 1% of an already-sorted series, as fps.
double low1_fps_of(const std::vector<double>& sorted_ms, std::size_t worst_n) {
    if (sorted_ms.empty() || worst_n == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = sorted_ms.size() - worst_n; i < sorted_ms.size(); ++i) {
        sum += sorted_ms[i];
    }
    const double mean_ms = sum / static_cast<double>(worst_n);
    return mean_ms > 0.0 ? 1000.0 / mean_ms : 0.0;
}

}  // namespace

FrameStats compute_frame_stats(std::span<const double> wall_ms,
                               std::span<const double> cpu_ms) {
    FrameStats s;
    if (wall_ms.empty()) return s;

    std::vector<double> sorted(wall_ms.begin(), wall_ms.end());
    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();
    s.frames = n;

    double sum = 0.0;
    for (double v : sorted) sum += v;
    s.avg_ms = sum / static_cast<double>(n);
    s.p50_ms = sorted[n / 2];
    s.p99_ms = sorted[std::min<std::size_t>(
        n - 1, static_cast<std::size_t>(static_cast<double>(n) * 0.99))];
    s.min_ms = sorted.front();
    s.max_ms = sorted.back();

    double var_sum = 0.0;
    for (double v : sorted) {
        const double d = v - s.avg_ms;
        var_sum += d * d;
    }
    s.stddev_ms =
        n > 1 ? std::sqrt(var_sum / static_cast<double>(n - 1)) : 0.0;

    const std::size_t worst_n = std::max<std::size_t>(1, n / 100);
    s.low1_fps = low1_fps_of(sorted, worst_n);

    for (double v : sorted) {
        if (v > kFrameBudgetMs) ++s.over_budget;
    }
    s.over_budget_pct =
        100.0 * static_cast<double>(s.over_budget) / static_cast<double>(n);

    // Frames without a paired CPU sample cannot be attributed either way,
    // so they keep their wall time and are never excused.
    const std::size_t paired = std::min(wall_ms.size(), cpu_ms.size());
    std::vector<double> engine_ms(wall_ms.begin(), wall_ms.end());
    for (std::size_t i = 0; i < paired; ++i) {
        if (!descheduled(wall_ms[i], cpu_ms[i])) continue;
        ++s.descheduled_frames;
        s.stolen_ms += wall_ms[i] - cpu_ms[i];
        engine_ms[i] = cpu_ms[i];
    }
    std::sort(engine_ms.begin(), engine_ms.end());
    s.engine_low1_fps = low1_fps_of(engine_ms, worst_n);
    return s;
}

}  // namespace core
