#pragma once

#include "world/block.h"
#include "world/chunk.h"

#include <cstdint>
#include <span>
#include <vector>

namespace world {

// An append-only log of block edits, and the ability to move a world to any
// point in its own history.
//
// The pattern is a write-ahead log with the world as its materialized
// state, which is how a database gets point-in-time recovery. Here it buys
// something a player can see: the world can be scrubbed backwards and
// forwards through everything that was ever built in it.
//
// It works at all only because this engine is deterministic. Terrain is a
// pure function of the seed (tests/test_world.cpp pins that: height_at is
// pure and two generators on one seed agree everywhere), so tick 0 costs
// nothing to store - it is the seed. Everything after tick 0 is this log.
//
// Each record carries the block that was there BEFORE the edit as well as
// the one after. That is one byte per edit, and it is what makes rewinding
// symmetric with replaying: undoing an edit is applying `prev` instead of
// `next`, so seeking backwards costs the same as seeking forwards and
// needs no snapshot to rewind from. Without it, stepping back one tick
// would mean regenerating terrain and replaying the entire log.
struct EditRecord {
    std::uint32_t tick = 0;
    std::int32_t  x = 0;
    std::int32_t  z = 0;
    std::uint8_t  y = 0;
    std::uint8_t  prev = 0;  // block id before the edit
    std::uint8_t  next = 0;  // block id after the edit
    std::uint8_t  reserved = 0;
};
// y is one byte because the world is exactly 256 blocks tall, so the field
// cannot hold an illegal height and the decoder needs no range check for
// it. Widen the world and that stops being true, which is what this
// catches - the decoder's comment points here.
static_assert(kChunkSizeY == 256,
              "EditRecord::y is a uint8_t chosen to exactly cover the world "
              "height; a taller world needs a wider field AND a range check "
              "in EditLog::decode");

static_assert(sizeof(EditRecord) == 16,
              "EditRecord is written to disk field by field, but its size "
              "is load-bearing for the reserve() in decode(); keep it a "
              "power of two so a log's byte size stays predictable");

// Binary log layout, same discipline as the chunk format:
//   magic[4]   "VLOG"
//   version    u8 = 1
//   reserved   u8[3] (zero)
//   seed       u32  the world this log belongs to; replaying a log against
//              a different seed would silently produce a world that never
//              existed, so the loader refuses rather than guesses
//   count      u32  number of records
//   crc        u32  CRC-32 (IEEE) over the header's first 16 bytes plus
//              every record byte, so a flipped seed or count is caught
//   records[]  count * 16 bytes, little-endian, tick-ordered
inline constexpr std::uint8_t kEditLogVersion = 1;
inline constexpr std::size_t  kEditLogHeaderBytes = 20;

class EditLog {
public:
    // Ticks must not go backwards. A log whose ticks are out of order
    // cannot be sought through correctly, and the failure is silent (you
    // get a world that never existed), so this refuses instead.
    bool record(std::uint32_t tick, int x, int y, int z,
                BlockId prev, BlockId next);

    std::size_t size() const { return records_.size(); }
    bool empty() const { return records_.empty(); }
    std::uint32_t latest_tick() const {
        return records_.empty() ? 0u : records_.back().tick;
    }
    std::span<const EditRecord> records() const { return records_; }
    void clear() { records_.clear(); }

    // Moves a world from `from_tick` to `to_tick`, calling
    // apply(x, y, z, block) for each change in the order it must happen.
    //
    // Forwards, records in (from, to] are applied in order using `next`.
    // Backwards, records in (to, from] are undone in REVERSE order using
    // `prev`. The reverse is not a detail: two edits to one cell only undo
    // correctly newest-first, because the older record's `prev` is the
    // state to end at and the newer one's has to be passed through first.
    //
    // Returns the number of applications made.
    template <typename Apply>
    std::size_t seek(std::uint32_t from_tick, std::uint32_t to_tick,
                     Apply&& apply) const {
        std::size_t applied = 0;
        if (to_tick > from_tick) {
            for (const EditRecord& r : records_) {
                if (r.tick <= from_tick) continue;
                if (r.tick > to_tick) break;
                apply(r.x, static_cast<int>(r.y), r.z,
                      static_cast<BlockId>(r.next));
                ++applied;
            }
        } else if (to_tick < from_tick) {
            for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
                if (it->tick > from_tick) continue;
                if (it->tick <= to_tick) break;
                apply(it->x, static_cast<int>(it->y), it->z,
                      static_cast<BlockId>(it->prev));
                ++applied;
            }
        }
        return applied;
    }

    std::vector<std::uint8_t> encode(std::uint32_t seed) const;

    // Rejects a truncated, corrupt, wrong-version or wrong-seed log rather
    // than loading part of one. `expected_seed` guards the case that would
    // otherwise be silent: a valid log replayed onto the wrong world.
    static bool decode(std::span<const std::uint8_t> bytes, EditLog& out,
                       std::uint32_t expected_seed);

    // The seed a log was written for, without decoding it. Returns false
    // if the bytes are not a well-formed log header.
    static bool peek_seed(std::span<const std::uint8_t> bytes,
                          std::uint32_t* out_seed);

private:
    std::vector<EditRecord> records_;
};

}  // namespace world
