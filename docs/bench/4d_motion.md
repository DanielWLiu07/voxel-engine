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
| travel w, walk (0.4 u/s) | 0.66 | 1.34 | 23.8 / 24 | 241 / 289 | 67 ms |
| travel w, sprint (1.2 u/s) | 0.63 | 1.10 | 24.0 / 24 | 289 / 289 | 109 ms |
| scroll 1 notch/s | 0.34 | 1.18 | **10.0 / 24** | **10 / 289** | 0 ms |
| scroll 5 notch/s | 0.68 | 1.34 | 23.8 / 24 | 112 / 289 | 37 ms |
| scroll 15 notch/s | 0.79 | 1.60 | 24.0 / 24 | 226 / 289 | 65 ms |
| scroll 60 notch/s | 0.77 | 1.97 | 24.0 / 24 | 273 / 289 | 84 ms |

The wheel is swept rather than measured at one rate, because a single
rate was actively misleading: the bench used 0.18 rad/s, which is sixty
notches a second, reported the whole window permanently stale, and that
said more about the rate chosen than about the engine.

Each phase resets the slice first and waits for the world to converge, or
it would measure the previous phase's leftovers. That reset streams 256
chunks an iteration rather than the default 24, and the difference is not
cosmetic: the convergence loop exits only when nothing is in flight AND
nothing is stale in the same pass, and at 24 a freshly-reset 625-chunk
world almost never satisfies both at once. It timed out, and the scroll
rows read 4 ms a frame and two seconds to settle - the reset, not the
wheel. The loop now says so out loud when it fails.

Read down the scroll rows against the two travel rows. A slow scroll is
the only motion here the stream fully absorbs: it issues 10 of its 24
chunk budget, leaves 10 of 289 chunks behind, and has nothing left to do
when the motion stops. A brisk deliberate turn at 15 notches/s costs
slightly less than walking (226 against 241 stale, 65 ms against 67). A
sixty-notch flick costs less than sprinting (273 against 289, 84 ms
against 109).

That is the design goal met: turning the slice is never more expensive
than travelling through it at a comparable pace, at any rate a hand can
produce.

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

**Staleness is a measured displacement, not a weighted guess**, and that
is why the scroll rows line up with the travel rows at all.

A chunk is stale when its terrain has moved far enough in the noise
field, and "far enough" is one number - a distance - for both motions.
The earlier form was `|dw| + |dtheta| * 32`, which had to invent a
constant to add an angle to a length, and 32 was picked to make the first
scroll notch clear the threshold rather than to describe anything. It was
wrong in both directions at once: a chunk sitting on the rotation axis
barely moves under a tilt and was marked stale anyway, while a distant
one moves far more than 32 implies.

Two things had to be true before a displacement could replace it. The
noise space had to be isotropic, or a given distance along w would mean
six times more change than the same distance along z and no single
threshold could serve both - that was a real defect, fixed separately.
And the rotation had to turn about the player rather than the world
origin, or the displacement of everything around a distant player would
be dominated by how far they had walked.

**Recovery is the number a player feels**, and it is under a tenth of a
second. This is the claim the whole streaming design exists to support, so
it is worth checking against something derived independently.
`./build/wcost 8` builds a cost model from first principles - generate
every chunk, mesh every chunk, no engine involved - and predicts:

    WCOST radius=8 chunks=289 full_step_ms=429.0
      chunk (today's mesher)   429.0 ms/step   48 ms on 9 workers

429 ms single-threaded for a full 289-chunk rebuild, 48 ms spread across
the pool. Sprinting leaves the whole window stale and converges in 109
ms, so 2.3x the model. That is the fair comparison - the faster rows
converge sooner because they left less of the window stale, not because
they beat the model - and the gap is the cost of not stopping the world:
the real path uploads on the one thread that owns the GL context,
re-meshes chunk boundaries as neighbours land, and spends the work at 24
chunks a frame by design.

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
