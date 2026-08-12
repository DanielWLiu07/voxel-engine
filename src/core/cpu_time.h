#pragma once

#include <ctime>

namespace core {

// Milliseconds of CPU time this thread has actually been executing for.
//
// Wall-clock frame time alone cannot tell the difference between an engine
// that is slow and an engine that was not running. A frame that takes 88 ms
// of wall time while the calling thread burns 4 ms of CPU was not a slow
// frame: the thread spent 84 ms off-core, descheduled by the OS because
// something else on the machine wanted the CPU. Reporting that as engine
// jitter measures the machine the benchmark happened to run on.
//
// CLOCK_THREAD_CPUTIME_ID is per-thread on purpose. RUSAGE_SELF sums every
// thread in the process, and this engine runs a nine-worker pool, so
// process CPU time routinely exceeds wall time and the comparison would be
// meaningless. The render loop's own thread is the one whose stalls show up
// as dropped frames.
//
// Available on macOS (10.12+) and Linux; both dev targets have it.
inline double thread_cpu_ms() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0.0;
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1.0e6;
}

}  // namespace core
