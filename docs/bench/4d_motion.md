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
| translate (0.4 w/s, walk speed) | 0.65-0.84 | 1.01-2.19 | 23.8 / 24 | **241 / 289** | 9-54 ms |
| rotate (0.18 rad/s, ~60 notches/s) | 0.68-0.78 | 0.91-1.70 | 24.0 / 24 | **289 / 289** | 22-34 ms |

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

**Recovery is the number a player feels**, and it is tens of
milliseconds. This is the claim the whole streaming design exists to
support, so it is worth checking against something that was derived
independently. `./build/wcost 8` builds a cost model from first
principles - generate every chunk, mesh every chunk, no engine involved -
and predicts:

    WCOST radius=8 chunks=289 full_step_ms=429.0
      chunk (today's mesher)   429.0 ms/step   48 ms on 9 workers

429 ms single-threaded for a full 289-chunk rebuild, 48 ms spread across
the pool. The measured settle after continuous rotation is 22-34 ms, at
or under that prediction, which is the check that matters: converging a
fully-stale window through the streaming path costs no more than a full
re-mesh would, and the streaming path spreads it over frames instead of
stopping the world for one.

## What this does not measure

Draw time. These are main-thread streaming costs, not frame times - the
engine is rendering throughout, but the numbers above deliberately isolate
the slice work so that a change to the streaming policy shows up here
undiluted by the renderer.

It also does not measure a rotation held long enough for the far edge of
the window to matter. `behind` is a count of resident chunks, and a chunk
past the fog contributes the same as one under the player's feet.
