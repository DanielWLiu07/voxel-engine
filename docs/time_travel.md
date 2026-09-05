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

## What this does not do yet

The log and its seek are complete and tested. Nothing is wired into the
engine's edit path or exposed as a flag, so no existing behaviour changes:
`--validate` still reports `gpu_mesh_mb=10.99` and `check_invariance.py`
still passes.

What wiring it up needs, in order: record into the log from
`World::set_block`, add periodic RLE checkpoints so a seek into a long
history does not replay from tick 0, and a `--replay` flag that scrubs.
The checkpoint work is the only part with a design question left in it,
and it is a familiar one - checkpoint interval against seek latency.
