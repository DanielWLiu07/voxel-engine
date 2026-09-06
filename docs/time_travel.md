# Time travel: a world you can scrub

The world can be moved to any point in its own history, forwards or
backwards, and land on exactly the world that existed at that moment.

The mechanism is a **write-ahead log with the world as its materialized
state**, which is how a database gets point-in-time recovery. Here it buys
something a player can see.

    world_state(t) = terrain(seed) + replay(log[0..t])

## Why it works here specifically

Because the engine is deterministic, which this repo already proves harder
than it needs to for any other reason.

Terrain is a pure function of the seed - `tests/test_world.cpp` pins that
`height_at` is pure and that two generators on one seed agree everywhere -
so **tick 0 costs nothing to store.** It is the seed. Everything after tick
0 is the log. The same determinism is what `scripts/check_invariance.py`
enforces in CI, and what makes the benchmark output byte-identical across
arm64 and x86-64.

A flashy feature falling out of the least flashy work in the repo is the
part worth pointing at.

## The design decision that matters

Each record carries the block that was there **before** the edit as well as
the one after:

```cpp
struct EditRecord {
    std::uint32_t tick;
    std::int32_t  x, z;
    std::uint8_t  y, prev, next, reserved;
};   // 16 bytes
```

That one extra byte is what makes rewinding symmetric with replaying.
Undoing an edit is applying `prev` instead of `next`, so seeking backwards
costs the same as seeking forwards and needs no snapshot to rewind from.
Without it, stepping back a single tick would mean regenerating terrain and
replaying the entire log from the beginning.

Backwards seeks walk the log in **reverse** order, and that is not a
detail. Two edits to one cell only undo correctly newest-first: the older
record's `prev` is the state to end at, and the newer record has to be
passed through on the way. Undone oldest-first, the cell ends holding a
value it passed through but never rested in.

## Cost

    1153 edits encode to 18468 bytes, 16.0 per edit

At 16 bytes a record, an hour of heavy building is well under a megabyte,
so there is never a reason to discard history. Measured by
`edit_log_tests`, which asserts the figure rather than just printing it.

## The file format

Same discipline as the chunk format, including the lesson that one learned
the hard way (v2 left the header outside the CRC, and a corrupted
edited-bit passed silently):

    magic[4]   "VLOG"
    version    u8 = 1
    reserved   u8[3]  zero
    seed       u32    the world this log belongs to
    count      u32    number of records
    crc        u32    CRC-32 over the first 16 header bytes + every record
    records[]  count * 16 bytes, little-endian, tick-ordered

The `seed` field guards the failure that would otherwise be completely
silent: a valid log replayed onto terrain from a different seed. Every edit
would land at a coordinate that means something else, and the result is a
world that looks fine and never existed. The decoder refuses rather than
guesses.

## What is verified

`tests/test_edit_log.cpp`, 67 checks. The central one is a property rather
than an example: build a random history while tracking the true world after
every tick with an oracle that never calls `seek()`, then for **every pair
of ticks** seek from one to the other and compare against the oracle.
Forwards, backwards, and to itself. Plus:

- the path taken does not change the destination (four seeks land where one
  seek lands)
- repeated edits to one cell rewind through every intermediate state in the
  right order
- the interval is half-open: a seek starting on a record's tick does not
  redo that record
- a log round-trips byte for byte, and the restored log replays to the same
  world
- corrupt, truncated, over-long, wrong-version, wrong-magic, wrong-seed and
  bit-flipped logs are all refused
- a failed decode leaves nothing behind

## Fault injection

Every check above was validated by breaking the code it covers. Twenty-two
faults; all are caught. Seven needed a second round, and those are the
interesting ones:

**Four header checks were passing for the wrong reason.** The CRC covers
the header, so it objected before the magic, version, reserved-byte or
count checks were ever reached - disabling any of them individually failed
nothing. The tests now repair the CRC after corrupting a field, so the
check under test is the only thing that can object.

**Two injections were landing on the wrong function.** `peek_seed` and
`decode` share their guard lines verbatim, so a patch aimed at `decode`
hit `peek_seed` first and `decode` kept working. That was a mis-aimed
injection rather than a weak test, but it exposed a real gap: `peek_seed`
is a separate code path and had no tests at all. It has five now.

**One check is not verifiable by assertion, and says so.** The length check
that stops a corrupt `count` from overrunning the buffer guards a
memory-safety property, not a behavioural one - with it removed, the
decoder reads past the end and whether the garbage happens to fail a later
check is luck. ASan is what proves it:

    heap-buffer-overflow ... edit_log.cpp:134 in EditLog::decode

with the check removed, clean with it in place. `run_sanitizers.sh` runs
this binary for that reason.

Two test bugs were found this way as well, both worth recording because
both looked correct:

- The out-of-order-tick case built its input by calling `record()` with a
  backwards tick, which `record()` correctly refused - leaving a
  one-record log, so the patch then wrote past the end of the buffer. The
  bug was in the test.
- A `y` range check in the decoder could never fire, because `y` is a
  `uint8_t` and the world is exactly 256 blocks tall, so every value the
  field can hold is legal. The test asserting `y = 200` was refused was
  simply wrong; 200 is a perfectly good height. The dead check is gone and
  a `static_assert` guards the assumption, so a taller world fails the
  build rather than silently gaining an unvalidated field.

## Wired into the engine

`World::set_block` records every accepted edit, one tick each. A refused
edit - setting a block to what it already is - consumes no tick, so the
timeline has no gaps that mean nothing.

`World::history_seek(tick)` moves the world. Recording is suspended for
the duration, because navigating history is not a new entry in it: without
that, scrubbing back and forth would append forever and the timeline would
drift away from what actually happened.

### Batching, which is where the work went

A seek applies every block change to voxel data first and remeshes each
touched chunk once at the end. The obvious implementation - calling
`set_block` in a loop - remeshes, relights and re-uploads per edit, so a
scrub across a thousand edits in one chunk costs a thousand rebuilds
instead of one.

Measured by `--verify-history`, which makes 301 edits and then rewinds
them, so both paths perform exactly the same block changes:

| | block changes | chunk remeshes | wall time (M4) |
| --- | --- | --- | --- |
| per edit (`set_block` loop) | 301 | 301 | 297-371 ms |
| batched (`history_seek`) | 301 | **14** | 13.1-13.9 ms |

**301 remeshes down to 14** is the durable figure: it is a ratio of
counts, so it reproduces on any machine. The wall-clock speedup is
**22.7-27.6x on an Apple M4** over four runs, quoted with its spread
because a timing without one is a number waiting to rot.

Fourteen rather than one because the edits are spread across a lattice
that crosses chunk boundaries, and a boundary edit remeshes the neighbour
too - the same rule `set_block` follows.

## Verified end to end

    ./build/voxel_engine --verify-history

    HISTORY edits=301 ticks=0->301 unbatched_ms=295.5 rewind_ms=13.2
    replay_ms=13.9 batch_speedup=22.5x rewind_applied=301
    rewind_remeshed=14 log_bytes=4836 bytes_per_edit=16.1
    bad_tris_rewind=0 bad_tris_replay=0 dropped=0
    main_ok=1 branch_ok=1 ok

It checks three things, and only the second genuinely needs GL:

- **Voxels**: an FNV-1a hash of every block in every resident chunk, in
  coordinate order. Not a spot check on the edited cells, so a rewind
  that restored the edits and corrupted a neighbouring chunk fails.
- **Meshes**: `debug_validate_gpu_meshes()`, reading every uploaded
  triangle back off the GPU. This is what makes the check need a context,
  and it was missing until a review proved the point - see below.
- **Branching**: an edit made while the world sits in its past, then
  rewound and replayed.

It is in `scripts/audit.sh` alongside the other end-to-end checks.

## Two defects an adversarial review found

Both were in the wiring rather than the log, both were silent, and both
are the reason `--verify-history` looks the way it does now.

### 1. An edit made after a rewind was applied to the world and dropped from the log

`set_block` discarded `EditLog::record`'s return value. The log is
tick-ordered, so a new edit recorded while the world sits in its own past
lands inside the existing log, and `record` refuses it. The block was
placed. The log never heard about it.

That produced two different wrong worlds, depending on how far back the
player had scrubbed:

- **Rewound more than one tick**: the new edit is refused outright. It
  becomes a block that exists in the world, is unreachable by any seek,
  and survives a full rewind to tick 0 - because nothing knows it is
  there.
- **Rewound exactly one tick**: `record` *accepts* it, because a tick
  equal to the last one is legal (one tick can hold many edits). It
  appends a duplicate tick, and scrubbing across that tick resurrects the
  edit the player just undid.

The fix is the behaviour every text editor has: an edit made in the past
discards the future first. `EditLog::truncate_after` drops the records
after the current tick, and `set_block` calls it before recording. The
return value is checked now, and a refusal increments
`World::history_dropped()`, which `--verify-history` asserts is zero -
the counter exists because ignoring that return is what allowed this.

### 2. `--verify-history` passed with the entire batching feature deleted

The check compared an FNV-1a hash of the voxel data before and after a
rewind. Voxel data only. Deleting the whole remesh loop from
`history_seek` left the hash matching, the flag printing `ok`, `audit.sh`
passing, and every rewound edit still on screen, because the GPU meshes
described a world that no longer existed.

The header comment claimed the check lived in the engine "because a
rewind remeshes and re-uploads every touched chunk, and that needs a GL
context". Nothing it asserted needed GL. The comment described the test
that should have been written.

It does now: `debug_validate_gpu_meshes()`, the same read-back
`--validate` uses, runs after the rewind and after the replay, and every
triangle has to be an axis-aligned face backed by a solid block. Re-run
the deletion and it reports `bad_tris_rewind=1338`, `FAILED`, exit 1.
`rewind.chunks_remeshed > 0` is asserted too, so a seek that changes
voxels and remeshes nothing cannot pass.

Both fixes were verified by restoring the original defects and confirming
the check fails: `dropped=1 branch_ok=0 FAILED` for the first,
`bad_tris_rewind=1338 FAILED` for the second.

## What this does not do yet

**Checkpoints.** A seek into a long history replays from wherever the
world currently sits, which is fine for scrubbing (you are usually moving
a short distance) and linear for a jump to the far end. Periodic RLE
snapshots would bound that, and the RLE codec already exists. The open
question is the familiar one: checkpoint interval against seek latency.

**Edits to chunks that streamed out.** Those are counted and reported as
`skipped_unloaded` rather than silently dropped, and they are already
preserved through `edited_stash_`, but a seek does not currently rewrite
the stash. So scrubbing works on the resident world and a chunk that was
away during a rewind comes back holding its latest state.

**A scrub UI.** There is no `--replay` flag or timeline slider yet; the
seek is an API and a verification flag.
