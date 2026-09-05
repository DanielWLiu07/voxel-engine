// Unit tests for the edit log, which is what lets a world be scrubbed
// backwards and forwards through its own history.
//
// The whole feature rests on one property: seeking to a tick has to give
// the world that actually existed at that tick, from wherever you started.
// Every failure mode here is silent by nature - a wrong seek does not
// crash, it hands you a plausible world that never existed - so these
// tests are mostly one invariant checked from several directions.
//
// The subtle case, and the reason undo runs the log backwards rather than
// forwards, is repeated edits to one cell. Undoing those oldest-first
// leaves the cell holding a state it never had.

#include "world/edit_log.h"

#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_serialize.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <tuple>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, label) do {                                            \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
        std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, label);      \
        ++g_failures;                                                       \
    }                                                                       \
} while (0)

// A stand-in for the world: sparse, so it can hold edits at any world
// coordinate without allocating a chunk grid. What matters for these tests
// is only which block sits at which cell.
using Key = std::tuple<int, int, int>;
using Blocks = std::map<Key, world::BlockId>;

auto applier(Blocks& blocks) {
    return [&blocks](int x, int y, int z, world::BlockId b) {
        blocks[{x, y, z}] = b;
    };
}

world::BlockId at(const Blocks& b, int x, int y, int z) {
    const auto it = b.find({x, y, z});
    return it == b.end() ? world::BlockId::Air : it->second;
}

// ----- the record contract --------------------------------------------------

void test_a_log_refuses_what_it_cannot_replay() {
    world::EditLog log;
    EXPECT(log.record(10, 0, 5, 0, world::BlockId::Air, world::BlockId::Stone),
           "an ordinary edit is recorded");
    // Ticks going backwards would make seek() walk the wrong records, and
    // the result would be a world that never existed rather than an error.
    EXPECT(!log.record(9, 0, 6, 0, world::BlockId::Air, world::BlockId::Stone),
           "a tick that goes backwards is refused");
    EXPECT(log.record(10, 0, 6, 0, world::BlockId::Air, world::BlockId::Stone),
           "the same tick again is fine - one tick can hold many edits");
    // A record that changes nothing replays and rewinds correctly and is
    // still wrong: it means the caller believed an edit happened.
    EXPECT(!log.record(11, 0, 7, 0, world::BlockId::Stone, world::BlockId::Stone),
           "an edit from a block to itself is refused");
    EXPECT(!log.record(11, 0, -1, 0, world::BlockId::Air, world::BlockId::Stone),
           "an edit below the world is refused");
    EXPECT(!log.record(11, 0, world::kChunkSizeY, 0,
                       world::BlockId::Air, world::BlockId::Stone),
           "an edit above the world is refused");
    EXPECT(log.size() == 2, "only the valid records were kept");
    EXPECT(log.latest_tick() == 10, "latest_tick is the last accepted tick");
}

// ----- seeking --------------------------------------------------------------

void test_replaying_forward_builds_the_world() {
    world::EditLog log;
    log.record(1, 4, 30, 4, world::BlockId::Air, world::BlockId::Stone);
    log.record(2, 5, 30, 4, world::BlockId::Air, world::BlockId::Wood);
    log.record(3, 6, 30, 4, world::BlockId::Air, world::BlockId::Glow);

    Blocks blocks;
    const std::size_t applied = log.seek(0, 3, applier(blocks));
    EXPECT(applied == 3, "all three edits applied");
    EXPECT(at(blocks, 4, 30, 4) == world::BlockId::Stone, "first edit landed");
    EXPECT(at(blocks, 5, 30, 4) == world::BlockId::Wood, "second edit landed");
    EXPECT(at(blocks, 6, 30, 4) == world::BlockId::Glow, "third edit landed");
}

void test_seeking_partway_stops_where_it_should() {
    world::EditLog log;
    log.record(1, 0, 30, 0, world::BlockId::Air, world::BlockId::Stone);
    log.record(5, 1, 30, 0, world::BlockId::Air, world::BlockId::Wood);
    log.record(9, 2, 30, 0, world::BlockId::Air, world::BlockId::Glow);

    Blocks blocks;
    log.seek(0, 5, applier(blocks));
    EXPECT(at(blocks, 0, 30, 0) == world::BlockId::Stone, "tick 1 applied");
    EXPECT(at(blocks, 1, 30, 0) == world::BlockId::Wood, "tick 5 applied");
    EXPECT(at(blocks, 2, 30, 0) == world::BlockId::Air, "tick 9 not yet applied");
    // The boundary is inclusive at `to` and exclusive at `from`, so seeking
    // to exactly a record's tick includes it. Off by one here would show up
    // as a scrub that lags the timeline by one edit.
    EXPECT(log.seek(5, 5, applier(blocks)) == 0, "seeking nowhere does nothing");

    // The interval is half-open, (from, to]. Starting a seek exactly on a
    // record's tick must not re-apply that record. Re-applying it would
    // set the cell to the value it already holds, so the world would look
    // right and only the work done would be wrong - which is why this is
    // checked by counting applications rather than by inspecting blocks.
    // Fault injection found it: changing `<= from` to `< from` in seek()
    // failed nothing until this line existed.
    Blocks scratch;
    log.seek(0, 5, applier(scratch));
    EXPECT(log.seek(5, 9, applier(scratch)) == 1,
           "a seek starting on a record's tick does not redo that record");
    EXPECT(log.seek(0, 9, applier(scratch)) == 3,
           "and a seek from the start does all three");
}

void test_rewinding_undoes_exactly_what_replaying_did() {
    world::EditLog log;
    log.record(1, 4, 30, 4, world::BlockId::Air,   world::BlockId::Stone);
    log.record(2, 5, 31, 4, world::BlockId::Air,   world::BlockId::Wood);
    log.record(3, 4, 30, 4, world::BlockId::Stone, world::BlockId::Glow);

    Blocks blocks;
    log.seek(0, 3, applier(blocks));
    log.seek(3, 0, applier(blocks));
    EXPECT(at(blocks, 4, 30, 4) == world::BlockId::Air, "cell is back to air");
    EXPECT(at(blocks, 5, 31, 4) == world::BlockId::Air, "and so is the other");
}

void test_repeated_edits_to_one_cell_rewind_in_the_right_order() {
    // The case the whole reverse iteration exists for. One cell, edited
    // three times. Undone oldest-first the cell ends up holding Wood - a
    // state it passed through but never ended a tick in from the outside.
    // Undone newest-first it ends up as Air, which is what was there.
    world::EditLog log;
    log.record(1, 0, 40, 0, world::BlockId::Air,   world::BlockId::Stone);
    log.record(2, 0, 40, 0, world::BlockId::Stone, world::BlockId::Wood);
    log.record(3, 0, 40, 0, world::BlockId::Wood,  world::BlockId::Glow);

    Blocks blocks;
    log.seek(0, 3, applier(blocks));
    EXPECT(at(blocks, 0, 40, 0) == world::BlockId::Glow, "ends as the last edit");

    log.seek(3, 2, applier(blocks));
    EXPECT(at(blocks, 0, 40, 0) == world::BlockId::Wood, "one step back is Wood");
    log.seek(2, 1, applier(blocks));
    EXPECT(at(blocks, 0, 40, 0) == world::BlockId::Stone, "two back is Stone");
    log.seek(1, 0, applier(blocks));
    EXPECT(at(blocks, 0, 40, 0) == world::BlockId::Air, "all the way back is Air");

    // And in one jump rather than three steps.
    Blocks jumped;
    log.seek(0, 3, applier(jumped));
    log.seek(3, 0, applier(jumped));
    EXPECT(at(jumped, 0, 40, 0) == world::BlockId::Air,
           "one long rewind matches three short ones");
}

// ----- the property that matters --------------------------------------------

// Builds a log of random edits, tracking the true world state after every
// tick so the test has an independent answer to compare against. This is
// the oracle: it never calls seek().
struct History {
    world::EditLog log;
    std::vector<Blocks> truth;  // truth[t] = the world after tick t
};

History random_history(std::uint32_t seed, int ticks) {
    History h;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> coord(-40, 40);
    std::uniform_int_distribution<int> height(1, 80);
    // A small block palette and a small coordinate range, deliberately:
    // collisions on the same cell are the interesting case and a wide
    // range would make them vanishingly rare.
    const world::BlockId palette[] = {world::BlockId::Air, world::BlockId::Stone,
                                      world::BlockId::Wood, world::BlockId::Glow,
                                      world::BlockId::Sand};
    std::uniform_int_distribution<int> pick(0, 4);

    Blocks current;
    h.truth.push_back(current);  // tick 0: before anything
    for (int t = 1; t <= ticks; ++t) {
        std::uniform_int_distribution<int> per_tick(0, 3);
        const int edits = per_tick(rng);
        for (int e = 0; e < edits; ++e) {
            const int x = coord(rng), y = height(rng), z = coord(rng);
            const world::BlockId prev = at(current, x, y, z);
            world::BlockId next = palette[pick(rng)];
            if (next == prev) continue;  // record() refuses a no-op edit
            h.log.record(static_cast<std::uint32_t>(t), x, y, z, prev, next);
            current[{x, y, z}] = next;
        }
        h.truth.push_back(current);
    }
    return h;
}

void test_seeking_anywhere_from_anywhere_gives_the_right_world() {
    // The invariant the feature lives on. For every pair of ticks, seeking
    // from one to the other must produce exactly the world the oracle
    // recorded at the destination - forwards, backwards, and to itself.
    for (std::uint32_t seed : {1u, 2u, 3u}) {
        const History h = random_history(seed, 40);
        int mismatches = 0;
        for (std::uint32_t from = 0; from <= 40; ++from) {
            for (std::uint32_t to = 0; to <= 40; ++to) {
                Blocks blocks;
                h.log.seek(0, from, applier(blocks));  // get to `from`
                h.log.seek(from, to, applier(blocks)); // then to `to`
                // Compare against the oracle over the union of both key
                // sets, so a cell the seek forgot to touch is caught as
                // well as one it set wrongly.
                for (const auto& [key, want] : h.truth[to]) {
                    const auto [x, y, z] = key;
                    if (at(blocks, x, y, z) != want) ++mismatches;
                }
                for (const auto& [key, got] : blocks) {
                    const auto [x, y, z] = key;
                    if (at(h.truth[to], x, y, z) != got) ++mismatches;
                }
            }
        }
        EXPECT(mismatches == 0, "every seek lands on the world that existed");
        if (mismatches != 0) {
            std::printf("    (seed %u: %d cell mismatches)\n", seed, mismatches);
        }
    }
}

void test_the_path_taken_does_not_change_the_destination() {
    // Scrubbing a timeline means arriving at a tick by many different
    // routes. Landing somewhere different depending on how you got there
    // would be invisible in a screenshot and obvious in use.
    const History h = random_history(7, 30);
    Blocks direct, wandered;
    h.log.seek(0, 20, applier(direct));

    h.log.seek(0, 30, applier(wandered));
    h.log.seek(30, 5, applier(wandered));
    h.log.seek(5, 27, applier(wandered));
    h.log.seek(27, 20, applier(wandered));

    int mismatches = 0;
    for (const auto& [key, want] : direct) {
        const auto [x, y, z] = key;
        if (at(wandered, x, y, z) != want) ++mismatches;
    }
    for (const auto& [key, got] : wandered) {
        const auto [x, y, z] = key;
        if (at(direct, x, y, z) != got) ++mismatches;
    }
    EXPECT(mismatches == 0, "four seeks land where one seek lands");
}

void test_seeking_past_the_end_is_the_same_as_seeking_to_the_end() {
    const History h = random_history(11, 20);
    Blocks at_end, past_end;
    h.log.seek(0, 20, applier(at_end));
    h.log.seek(0, 9999, applier(past_end));
    int mismatches = 0;
    for (const auto& [key, want] : at_end) {
        const auto [x, y, z] = key;
        if (at(past_end, x, y, z) != want) ++mismatches;
    }
    EXPECT(mismatches == 0, "seeking beyond the log stops at the last edit");
}

// ----- persistence ----------------------------------------------------------

void test_a_log_survives_a_round_trip() {
    const History h = random_history(5, 60);
    const auto bytes = h.log.encode(1337);

    world::EditLog restored;
    EXPECT(world::EditLog::decode(bytes, restored, 1337), "the log decodes");
    EXPECT(restored.size() == h.log.size(), "record count survives");

    bool identical = true;
    for (std::size_t i = 0; i < restored.size(); ++i) {
        const auto& a = h.log.records()[i];
        const auto& b = restored.records()[i];
        if (a.tick != b.tick || a.x != b.x || a.y != b.y || a.z != b.z ||
            a.prev != b.prev || a.next != b.next) {
            identical = false;
        }
    }
    EXPECT(identical, "every field of every record survives byte for byte");

    // And the restored log has to seek identically, which is the thing
    // actually being persisted - matching fields would be worth nothing if
    // the replay came out different.
    Blocks from_original, from_restored;
    h.log.seek(0, 60, applier(from_original));
    restored.seek(0, 60, applier(from_restored));
    int mismatches = 0;
    for (const auto& [key, want] : from_original) {
        const auto [x, y, z] = key;
        if (at(from_restored, x, y, z) != want) ++mismatches;
    }
    EXPECT(mismatches == 0, "the restored log replays to the same world");
}

void test_an_empty_log_round_trips() {
    const world::EditLog empty;
    world::EditLog restored;
    EXPECT(world::EditLog::decode(empty.encode(42), restored, 42),
           "an empty log is still a valid log");
    EXPECT(restored.empty(), "and decodes to nothing");
    EXPECT(restored.latest_tick() == 0, "with tick 0");
}

void test_a_failed_decode_leaves_nothing_behind() {
    // A decoder that fails after partially filling its output hands the
    // caller a half-loaded history that looks like a real one. The
    // `restored.empty()` checks elsewhere cannot see this, because they
    // use a fresh EditLog that was empty to begin with - fault injection
    // showed that deleting the clear() at the top of decode() failed
    // nothing. This reuses a log that already holds records.
    const History h = random_history(6, 20);
    world::EditLog log;
    EXPECT(world::EditLog::decode(h.log.encode(1337), log, 1337),
           "the good log loads");
    EXPECT(log.size() == h.log.size(), "and holds its records");

    auto truncated = h.log.encode(1337);
    truncated.pop_back();
    EXPECT(!world::EditLog::decode(truncated, log, 1337),
           "the corrupt log is refused");
    EXPECT(log.empty(),
           "and the previously loaded history is cleared, not left stale");
}

void test_a_log_refuses_the_wrong_world() {
    // The silent failure this exists to prevent: a perfectly valid log
    // replayed onto terrain from a different seed. Every edit would land
    // at a coordinate that means something else, and the result is a
    // world that looks fine and never existed.
    const History h = random_history(3, 10);
    const auto bytes = h.log.encode(1337);
    world::EditLog restored;
    EXPECT(!world::EditLog::decode(bytes, restored, 1338),
           "a log for another seed is refused");
    EXPECT(restored.empty(), "and nothing is left half-loaded");
    std::uint32_t seed = 0;
    EXPECT(world::EditLog::peek_seed(bytes, &seed) && seed == 1337,
           "the seed can be read without decoding");

    // peek_seed does its own header validation, and it is a separate code
    // path from decode() with the same three checks written out again.
    // Worth testing on its own: fault injection aimed at decode() kept
    // landing on peek_seed instead, because the two share their guard
    // lines verbatim, and neither had a test that could tell them apart.
    std::uint32_t ignored = 0;
    EXPECT(!world::EditLog::peek_seed({}, &ignored),
           "peek_seed refuses an empty buffer");
    EXPECT(!world::EditLog::peek_seed(std::span(bytes).first(8), &ignored),
           "peek_seed refuses a buffer too short to hold a header");
    {
        auto b = bytes; b[0] = 'X';
        EXPECT(!world::EditLog::peek_seed(b, &ignored),
               "peek_seed refuses wrong magic");
    }
    {
        auto b = bytes; b[4] = 99;
        EXPECT(!world::EditLog::peek_seed(b, &ignored),
               "peek_seed refuses an unknown version");
    }
    EXPECT(world::EditLog::peek_seed(bytes, nullptr),
           "peek_seed tolerates a null out pointer");
}

// Recomputes a log's CRC so that a deliberately corrupted field is the
// only thing wrong with it. Without this, every header test passes for the
// same reason - the CRC covers the header, so it objects first and the
// specific check being tested is never exercised. Fault injection is what
// exposed that: disabling the magic, version and reserved-byte checks
// individually failed nothing.
void repair_crc(std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> crc_input;
    crc_input.insert(crc_input.end(), bytes.begin(), bytes.begin() + 16);
    crc_input.insert(crc_input.end(),
                     bytes.begin() + world::kEditLogHeaderBytes, bytes.end());
    const std::uint32_t crc = world::crc32_ieee(crc_input.data(), crc_input.size());
    bytes[16] = static_cast<std::uint8_t>(crc & 0xFFu);
    bytes[17] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
    bytes[18] = static_cast<std::uint8_t>((crc >> 16) & 0xFFu);
    bytes[19] = static_cast<std::uint8_t>((crc >> 24) & 0xFFu);
}

void test_corrupt_logs_are_refused() {
    const History h = random_history(4, 25);
    const auto good = h.log.encode(1337);

    auto refuses = [&](std::vector<std::uint8_t> bytes, const char* label) {
        world::EditLog out;
        EXPECT(!world::EditLog::decode(bytes, out, 1337), label);
    };
    // Same, but with the checksum made valid first, so the named field is
    // the only thing the decoder can be objecting to.
    auto refuses_with_valid_crc = [&](std::vector<std::uint8_t> bytes,
                                      const char* label) {
        repair_crc(bytes);
        world::EditLog out;
        EXPECT(!world::EditLog::decode(bytes, out, 1337), label);
    };

    refuses({}, "empty input is refused");
    refuses({'V', 'L', 'O', 'G'}, "a header-only truncation is refused");
    {
        auto b = good; b[0] = 'X';
        refuses_with_valid_crc(b, "wrong magic is refused on its own merits");
    }
    {
        auto b = good; b[4] = 99;
        refuses_with_valid_crc(b, "an unknown version is refused on its own merits");
    }
    {
        auto b = good; b[6] = 1;
        refuses_with_valid_crc(b, "a dirty reserved byte is refused on its own merits");
    }
    {
        auto b = good; b.pop_back();
        refuses(b, "a truncated final record is refused");
    }
    {
        auto b = good; b.push_back(0);
        refuses(b, "trailing junk is refused");
    }
    {
        // A single flipped bit, placed in a record's x coordinate. That
        // field has no structural constraint - any int32 is a legal world
        // coordinate - so nothing but the CRC can object.
        //
        // The first version of this flipped a bit in a tick field, and
        // fault injection showed it was passing for the wrong reason:
        // with the CRC check disabled the log was still refused, because
        // the corrupted tick tripped the ordering check instead. A test
        // that names the CRC has to fail when only the CRC is removed.
        auto b = good;
        b[world::kEditLogHeaderBytes + 16 + 5] ^= 0x40;  // record 1, x byte 1
        refuses(b, "a flipped bit in a coordinate is caught by the CRC");
    }
    {
        // Count says more records than the bytes hold. The CRC catches
        // this one, since the count is under the CRC.
        auto b = good; b[12] = static_cast<std::uint8_t>(b[12] + 1);
        refuses(b, "a count that disagrees with the length is refused");
    }
    {
        // The dangerous version of the same thing: an inflated count with
        // a CRC repaired to match, so the checksum is genuinely valid and
        // only the length check stands between the decoder and reading
        // far past the end of the buffer.
        //
        // Fault injection is what put this here. Disabling the length
        // check alone failed nothing, because every other malformed-length
        // case was being caught by the CRC further down.
        //
        // This case is not verified by this assertion alone, and that is
        // worth being honest about: with the length check removed, the
        // decoder reads past the buffer and whether the garbage it finds
        // happens to fail a later check is luck. What actually proves the
        // guard is ASan, which reports
        //
        //     heap-buffer-overflow ... edit_log.cpp:134 in EditLog::decode
        //
        // on this input with the check removed, and is clean with it in
        // place. The sanitizer suite runs this binary for that reason.
        auto b = good;
        b[12] = 0xFF; b[13] = 0xFF; b[14] = 0; b[15] = 0;  // count = 65535
        refuses_with_valid_crc(b,
            "an inflated count with a valid CRC is refused on length");
    }
}

void test_a_crc_valid_log_can_still_be_unreplayable() {
    // Passing the CRC only proves the bytes are the bytes that were
    // written. It says nothing about whether they describe a history this
    // build can replay, so the decoder re-checks the rules record()
    // enforces. Built by writing a valid two-record log and then patching
    // the second record's bytes, because record() would refuse these
    // inputs at the door - the point is to get them past the CRC.
    //
    // The first version of this helper tried to create the out-of-order
    // case by calling record() with a backwards tick, which record()
    // correctly refused. That left a one-record log, and the patch then
    // wrote past the end of the buffer. The bug was in the test.
    constexpr std::size_t kSecond = world::kEditLogHeaderBytes + 16;

    auto patched = [&](auto&& patch) {
        world::EditLog log;
        log.record(1, 0, 30, 0, world::BlockId::Air, world::BlockId::Stone);
        log.record(2, 0, 31, 0, world::BlockId::Air, world::BlockId::Wood);
        auto bytes = log.encode(1337);
        patch(bytes);
        // Repair the CRC so the only thing wrong is the content.
        std::vector<std::uint8_t> crc_input;
        crc_input.insert(crc_input.end(), bytes.begin(), bytes.begin() + 16);
        crc_input.insert(crc_input.end(),
                         bytes.begin() + world::kEditLogHeaderBytes, bytes.end());
        const std::uint32_t crc =
            world::crc32_ieee(crc_input.data(), crc_input.size());
        bytes[16] = static_cast<std::uint8_t>(crc & 0xFFu);
        bytes[17] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
        bytes[18] = static_cast<std::uint8_t>((crc >> 16) & 0xFFu);
        bytes[19] = static_cast<std::uint8_t>((crc >> 24) & 0xFFu);
        return bytes;
    };

    world::EditLog out;
    // The control first: the same construction, unpatched, must decode. If
    // it did not, every rejection below would pass for the wrong reason.
    EXPECT(world::EditLog::decode(patched([](auto&) {}), out, 1337),
           "an unpatched two-record log decodes");
    EXPECT(out.size() == 2, "and holds both records");

    EXPECT(!world::EditLog::decode(
               patched([&](auto& b) { b[kSecond + 13] = 99; }), out, 1337),
           "an unknown block id is refused despite a valid CRC");
    EXPECT(!world::EditLog::decode(
               patched([&](auto& b) { b[kSecond + 14] = b[kSecond + 13]; }),
               out, 1337),
           "a no-op edit is refused despite a valid CRC");
    EXPECT(!world::EditLog::decode(
               patched([&](auto& b) { b[kSecond + 0] = 0; }), out, 1337),
           "a tick that goes backwards is refused despite a valid CRC");
    EXPECT(!world::EditLog::decode(
               patched([&](auto& b) { b[kSecond + 15] = 1; }), out, 1337),
           "a non-zero reserved byte is refused despite a valid CRC");

    // No "y out of range" case, on purpose. y is a uint8_t and the world
    // is 256 blocks tall, so every value the field can hold is a legal
    // height - there is nothing to reject. The first draft asserted y=200
    // was refused, which was simply wrong. A static_assert in edit_log.h
    // guards the assumption instead, and fires if the world gets taller.
    EXPECT(world::EditLog::decode(
               patched([&](auto& b) { b[kSecond + 12] = 255; }), out, 1337),
           "the highest block in the world is a legal edit");
}

void test_the_log_is_small() {
    // History being cheap is what makes keeping all of it reasonable. At
    // 16 bytes a record, an hour of heavy building is well under a
    // megabyte, so there is never a reason to throw any of it away.
    const History h = random_history(9, 1000);
    const auto bytes = h.log.encode(1337);
    const double per_record =
        static_cast<double>(bytes.size()) / static_cast<double>(h.log.size());
    EXPECT(per_record < 17.0, "a recorded edit costs about 16 bytes");
    std::printf("  (%zu edits encode to %zu bytes, %.1f per edit)\n",
                h.log.size(), bytes.size(), per_record);
}

}  // namespace

int main() {
    std::printf("edit_log_tests: running...\n\n");
    test_a_log_refuses_what_it_cannot_replay();
    test_replaying_forward_builds_the_world();
    test_seeking_partway_stops_where_it_should();
    test_rewinding_undoes_exactly_what_replaying_did();
    test_repeated_edits_to_one_cell_rewind_in_the_right_order();
    test_seeking_anywhere_from_anywhere_gives_the_right_world();
    test_the_path_taken_does_not_change_the_destination();
    test_seeking_past_the_end_is_the_same_as_seeking_to_the_end();
    test_a_log_survives_a_round_trip();
    test_an_empty_log_round_trips();
    test_a_log_refuses_the_wrong_world();
    test_a_failed_decode_leaves_nothing_behind();
    test_corrupt_logs_are_refused();
    test_a_crc_valid_log_can_still_be_unreplayable();
    test_the_log_is_small();

    std::printf("\nedit_log_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
