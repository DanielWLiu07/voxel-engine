# Benchmark & profiling artifacts

Committed measurements, each reproducible from the repo. Deterministic counts
(triangles, drawn sections, cull ratios) are machine-independent and CI-gated;
wall-clock figures depend on machine load, so they ship with their variance.

## `queue_bench.txt` — lock-free vs mutex queue

Throughput/latency sweep of `core/mpmc_queue.h` against a `std::mutex +
std::queue` pool across contention and per-item granularity. Shows the
lock-free queue winning 2–5× under contention but converging with the mutex
pool as per-item work approaches the engine's ~1 ms chunk-job size — the basis
for keeping the live streaming pool mutex-based.

```
cmake --build build --target queue_bench
./build/queue_bench > docs/bench/queue_bench.txt
```

## `frame_variance.txt` — frame-time run-to-run distribution

10 repeats of the 300-frame bench, reporting mean / stddev / min / max /
coefficient of variation. Quantifies measurement noise so a quoted frame time
can be defended rather than cherry-picked.

```
scripts/bench_variance.sh 10 300 center > docs/bench/frame_variance.txt
```

## `frame_capture.tracy` + `frame_zones.csv` — profiler capture

A real Tracy trace of a 600-frame center-pose run (671 frames, ~5.9 k zones),
plus a CSV export of per-zone time. Open the `.tracy` in the Tracy GUI for the
flamegraph, or read the CSV for the same data headlessly. Top zones in this
capture: `chunk_worker_job` 36.7 %, `mesh_greedy` 19 %, `shadow_pass` 13 %,
`postfx_resolve` 8.7 %, `terrain_pass` 7.7 %, `occlusion_bfs` 7.6 %.

Reproduce the capture headlessly (no GUI needed):

```
# 1. Build the engine with Tracy instrumentation.
cmake -B build-tracy -G Ninja -DCMAKE_BUILD_TYPE=Release -DVOXEL_USE_TRACY=ON
cmake --build build-tracy --target voxel_engine

# 2. Build Tracy's headless capture + csv tools. (The bundled freetype needs
#    the CMake policy shim on CMake 4+.)
TS=build-tracy/_deps/tracy-src
cmake -B build-tracy-tools/capture   -S "$TS/capture"   -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake -B build-tracy-tools/csvexport -S "$TS/csvexport" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-tracy-tools/capture
cmake --build build-tracy-tools/csvexport

# 3. Run the bench holding at exit until the profiler pulls the trace, and
#    capture it from a second shell.
TRACY_NO_EXIT=1 ./build-tracy/voxel_engine --bench-frame 600 --pose center &
build-tracy-tools/capture/tracy-capture -o docs/bench/frame_capture.tracy -a 127.0.0.1 -f
build-tracy-tools/csvexport/tracy-csvexport docs/bench/frame_capture.tracy > docs/bench/frame_zones.csv
```
