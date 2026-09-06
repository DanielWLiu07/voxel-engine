# What moving through the fourth dimension costs

    ./build/voxel_engine --bench-4d --radius 8

The engine's other benchmarks measure a **static** world: how fast it
draws, how much memory the meshes take, how much the culler removes.
None of them touch the thing that actually makes a 4D engine hard.

In a 3D engine, moving is cheap because the geometry does not change - the
camera moves through a world that is already meshed. In a 4D engine the
screen shows a 3D slice of a 4D world, so moving through the fourth axis
**invalidates geometry**. Every chunk's contents change. The naive
implementation of that is a whole-world re-mesh per step, which is exactly
the thing that would make a 4D voxel engine unplayable.

This measures both motions through w under the same 60 Hz pacing the
render loop uses. Apple M4, 9-worker pool, radius 8 (289 chunks).

| motion | mean ms | p99 ms | issued/frame | behind when motion stopped | settle |
|---|---|---|---|---|---|
| translate (0.4 w/s, walk speed) | 0.63-0.74 | 0.78-1.10 | 23.8 / 24 | **241 / 289** | 68-79 ms |
| rotate (0.18 rad/s, ~60 notches/s) | 0.67-0.73 | 1.11-1.60 | 24.0 / 24 | **289 / 289** | 87-117 ms |

Three runs each; timings are given as ranges because they are timings.
The two count columns are not - `issued/frame` and `behind` reproduce
exactly, run to run, because they are counts of chunks rather than
measurements of a machine.

## Reading it

**The main-thread cost is about 4-5% of a 60 Hz frame.** 0.65-0.84 ms of
16.67, for the whole per-frame slice job: decide what is stale, issue the
rebuilds, drain finished chunks, flush boundary re-meshes. Generation and
meshing are on the worker pool; this is what the thread that owns the GL
context pays.

**There is no chunks/sec figure here, deliberately.** The first version of
this bench had one, and it read 1430 for translation and 1439 for
rotation - which is 24 x 60, the per-frame streaming budget times the
frame rate, to three digits. Both motions saturate the budget on every
frame, so the column was reporting a constant that was already in the
source, dressed as a measurement. It would have been quoted as throughput.

**Saturating the budget is the expected state, not a failure.** It means
the motion invalidates geometry faster than a bounded stream replaces it,
which is true of any continuous motion through w and is the reason the
stream is bounded in the first place: an unbounded one would submit a
whole window of jobs per frame and discard most of them on arrival. The
question a bounded stream has to answer is not "does it keep up" - it
cannot, by construction - but "how far behind does it get, and how fast
does it recover".

**Rotation invalidates more than translation**, and the `behind` column is
where that shows: 289 of 289 chunks against 241. That matches the
generator-level measurement (`slice4d --tilt`): one scroll notch changes
63.8% of terrain columns, where translating a whole rebuild threshold
changes 36.3%.

**Recovery is the number a player feels**, and it is under a tenth of a
second. This is the claim the whole streaming design exists to support, so
it is worth checking against something derived independently.
`./build/wcost 8` builds a cost model from first principles - generate
every chunk, mesh every chunk, no engine involved - and predicts:

    WCOST radius=8 chunks=289 full_step_ms=429.0
      chunk (today's mesher)   429.0 ms/step   48 ms on 9 workers

429 ms single-threaded for a full 289-chunk rebuild, 48 ms spread across
the pool. Measured convergence is 68-117 ms, so 1.4x to 2.4x the model.

That is the right shape, and a settle FASTER than the model would have
meant the model was wrong rather than the engine fast. The model assumes
perfect 9-way parallelism and counts only generation and meshing. The real
path also uploads meshes to the GPU on the one thread that owns the
context, re-meshes chunk boundaries as neighbours land, and spends the
work at 24 chunks a frame by design instead of dumping all 289 into the
pool at once. Two of those three are the cost of not stopping the world,
which is the whole point.

### The settle figure was wrong twice before it was right

Both mistakes produced plausible small numbers, which is what makes them
worth recording.

The first version reported 22-34 ms and the loop had never converged at
all. `enqueue_decoded_chunk` did not stamp the slice on the job it
submitted, so every restored and re-meshed chunk landed claiming slice
(w=0, theta=0), was instantly stale again at any other slice, and was
re-issued forever. The bench reported the time it took to give up. It now
prints `NEVER (n)` rather than a bare number when the loop exits on its
deadline, because a settle that times out must not be able to look like a
fast settle.

The second was the loop bound itself: `for (guard = 0; guard < 4000)` is a
spin count, not a timeout. With nothing finished to drain the body takes a
couple of microseconds, so 4000 iterations elapsed in 8 ms while the nine
workers had barely started, and the loop exited with 194 of 289 chunks
still stale. It measured how long it takes to spin 4000 times - a number
that is stable, reproducible, and meaningless. Bounded by a deadline now.

The same distinction appears twice in `main.cpp` and only one of them is a
bug: an iteration guard is fine when the body cannot no-op. `--verify-4d`
calls a deadline-bounded drain first, so each of its iterations is a
completed drain cycle rather than a spin.

## What this does not measure

Draw time. These are main-thread streaming costs, not frame times - the
engine is rendering throughout, but the numbers above deliberately isolate
the slice work so that a change to the streaming policy shows up here
undiluted by the renderer.

It also does not measure a rotation held long enough for the far edge of
the window to matter. `behind` is a count of resident chunks, and a chunk
past the fog contributes the same as one under the player's feet.
