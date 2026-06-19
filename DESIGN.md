# Design notes

How the engine is layered, how work crosses threads, and how the performance
numbers are measured.

## Layering

```
core/   window, input, time, thread pool, lock-free queue   (no project deps)
gfx/    shader, mesh, camera, frustum, CSM, post-process, water, atlas
world/  chunk, terrain gen, greedy mesher, streaming, RLE serialize
render/ lighting + draw passes composed from gfx/ and world/
game/   player, AABB physics, block interaction
ui/     debug HUD
```

The dependency rule is one-directional and enforced by review: `gfx/` is a
generic OpenGL wrapper that knows nothing about voxels; `world/` owns voxel data
and uses `gfx/` to upload meshes but never reaches into gameplay; `game/` is the
only layer allowed to coordinate world + player + input. There are no globals;
the only singleton is the logger. Keeping `gfx/` voxel-agnostic also lets the
mesher and the renderer be benchmarked on their own.

## Threading model

Chunk generation is embarrassingly parallel and expensive (~1 ms per chunk:
Perlin fill + greedy mesh + section-visibility flood fill). OpenGL is not
thread-safe on a single context. So the split is:

- **Worker pool** (`core/ThreadPool`, one thread per core minus a couple) runs
  the CPU-heavy chunk pipeline. Each job produces a `FinishedChunk` (raw blocks
  + CPU-side mesh data + visibility bitsets) and pushes it onto a result queue.
- **Main thread** owns the GL context exclusively. Once per frame it drains a
  bounded number of finished chunks and performs the only GPU-touching step -
  the VBO/VAO upload (`build_slot`). Capping uploads per frame keeps a burst of
  completed chunks from stalling a frame.

Crossing this boundary is the most bug-prone part of the engine, so it is kept
narrow: workers never touch GL, the main thread never meshes.

### Why the result queue is a mutex, not lock-free

The streaming path looks like an obvious candidate for a lock-free queue, and
the engine ships one (`core/mpmc_queue.h`, a Vyukov bounded MPMC queue with a
concurrency stress test). I benchmarked it against the mutex pool first
(`bench/queue_bench`, output in `docs/bench/queue_bench.txt`):

- Under contention at fine granularity the lock-free queue beats a
  `std::mutex + std::queue` pool by 2-5x.
- That edge vanishes as per-item work grows. A chunk job is ~1 ms; a queue
  operation is tens of nanoseconds. At that ratio the queue is never the
  bottleneck and the two are indistinguishable.

So the live pool stays mutex-based, and the lock-free queue stays as a tested
component rather than a claim the engine leans on.

## The three performance levers

1. **Greedy meshing.** Co-planar identical block faces merge into the largest
   possible rectangles, cutting the triangle count the GPU sees. Measured
   **18.1x** fewer quads vs naive face culling on contiguous Perlin terrain (the
   CI-gated number), dropping to ~7.8x once caves fragment the face runs. The
   mesher's output is asserted area-equivalent to the naive mesher, including
   adversarial cases (checkerboard = zero legal merges; two-material slab =
   merges must split on block id).

2. **Hierarchical culling.** Per-chunk frustum AABBs, then 32-block section
   AABBs, then a Minecraft-style occlusion BFS over a per-section
   air-connectivity graph. Frustum-only culling ceilings near 3x at 70 deg FOV;
   occlusion is what produces the big wins underground (**70.8x** fewer drawn
   sections from inside a cave). A line-of-sight ray test guarantees the
   occlusion cull never removes geometry the camera can legitimately see.

3. **Async streaming.** The worker pool sustains ~2,200 chunks/sec; the
   main-thread upload is a small fraction of that wall time. Memory stays
   bounded under infinite streaming because out-of-window chunks are evicted and
   in-flight results for evicted chunks are dropped on drain.

## Measurement methodology

- **Load-independent vs wall-clock.** Drawn-section counts, triangle counts, and
  cull ratios are deterministic - same binary, same camera pose, same numbers on
  any machine. Frame time is not; it depends on machine load. The README labels
  which is which, and the CI gates only assert the deterministic ratios so they
  never flake on a busy runner.
- **Deterministic poses.** `--bench` locks the camera to a fixed pose (position,
  yaw, pitch, FOV, aspect) and freezes shader time, so a screenshot is
  byte-stable. That same determinism is how the occlusion culler is verified: an
  unculled render and a culled render must be pixel-identical.
- **Benchmarks.** `--bench` runs the mesher + cull benchmarks (CI-gated);
  `--bench-frame N` collects N vsync-off frame samples at a pose;
  `scripts/bench_sweep.sh` sweeps `kStreamRadius` and restores it on exit.
  `--pass-breakdown` glFinish-brackets each render pass for real per-pass wall
  time - at the cost of inflating frame-level avg by ~2.7x, so that mode is a
  diagnostic and its avg is never quoted as the frame time.
- **Profiling.** Tracy instrumentation is compiled in behind
  `-DVOXEL_USE_TRACY=ON`; the worker job, mesh build, and frame drain are zoned
  so a capture attributes time to the subsystem that spent it.
- **Reproducibility.** Every headline number on the README is reproducible from
  one of the commands above on an idle machine.
- **Sanitizers.** `scripts/run_sanitizers.sh` (CI "sanitizers" job) runs the
  lock-free queue + worker pool under ThreadSanitizer and the full logic suite
  (mesher, RLE codec + fuzz cases) under AddressSanitizer + UndefinedBehavior
  Sanitizer. The concurrency code is TSan-clean, so the queue's atomic memory
  ordering holds under load. The only UBSan suppression is FastNoiseLite's
  intentional hash wraparound; our own code is clean.
