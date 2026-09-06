#include "world/edit_log.h"

#include "world/chunk.h"
#include "world/chunk_serialize.h"

#include <algorithm>
#include <cstring>

namespace world {

namespace {

constexpr char kMagic[4] = {'V', 'L', 'O', 'G'};

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Written and read field by field rather than by memcpy of the struct, so
// the file does not inherit the compiler's padding and stays readable on a
// machine with different alignment rules. The static_assert on
// sizeof(EditRecord) is about memory, not about this.
constexpr std::size_t kRecordBytes = 16;

}  // namespace

bool EditLog::record(std::uint32_t tick, int x, int y, int z,
                     BlockId prev, BlockId next) {
    // Tick 0 is the world before anything was built, and seek() can never
    // reach a record stored there: forward skips `tick <= from_tick` and a
    // seek starts at 0 or later, backward breaks on `tick <= to_tick` and
    // a seek ends at 0 or later. A tick-0 record would sit in the log
    // looking like a stored edit and never replay in either direction.
    // This class refuses what it cannot replay, and that is the one input
    // that slipped through.
    if (tick == 0) return false;
    if (!records_.empty() && tick < records_.back().tick) return false;
    if (y < 0 || y >= kChunkSizeY) return false;
    const auto p = static_cast<std::uint8_t>(prev);
    const auto n = static_cast<std::uint8_t>(next);
    if (p > kMaxBlockId || n > kMaxBlockId) return false;
    // A record that changes nothing would replay and rewind correctly and
    // still be wrong: it inflates the log and, more importantly, means the
    // caller believed an edit happened when none did.
    if (p == n) return false;
    records_.push_back(EditRecord{tick, static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(z),
                                  static_cast<std::uint8_t>(y), p, n, 0});
    return true;
}

std::size_t EditLog::truncate_after(std::uint32_t tick) {
    // Records are tick-ordered, so everything to drop is a suffix.
    const auto first_after = std::find_if(
        records_.begin(), records_.end(),
        [tick](const EditRecord& r) { return r.tick > tick; });
    const std::size_t dropped =
        static_cast<std::size_t>(records_.end() - first_after);
    records_.erase(first_after, records_.end());
    return dropped;
}

std::vector<std::uint8_t> EditLog::encode(std::uint32_t seed) const {
    std::vector<std::uint8_t> out;
    out.reserve(kEditLogHeaderBytes + records_.size() * kRecordBytes);
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(kEditLogVersion);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    put_u32(out, seed);
    put_u32(out, static_cast<std::uint32_t>(records_.size()));
    const std::size_t crc_at = out.size();
    put_u32(out, 0);  // placeholder, filled once the body exists

    for (const EditRecord& r : records_) {
        put_u32(out, r.tick);
        put_u32(out, static_cast<std::uint32_t>(r.x));
        put_u32(out, static_cast<std::uint32_t>(r.z));
        out.push_back(r.y);
        out.push_back(r.prev);
        out.push_back(r.next);
        out.push_back(0);
    }

    // The CRC covers the header ahead of it and every record byte, so a
    // flipped seed or count is caught the same way a flipped block id is.
    // The chunk format learned this the hard way: v2 left the header out
    // and a corrupted edited-bit passed silently.
    std::vector<std::uint8_t> crc_input;
    crc_input.reserve(out.size());
    crc_input.insert(crc_input.end(), out.begin(), out.begin() + crc_at);
    crc_input.insert(crc_input.end(), out.begin() + kEditLogHeaderBytes, out.end());
    const std::uint32_t crc = crc32_ieee(crc_input.data(), crc_input.size());
    out[crc_at + 0] = static_cast<std::uint8_t>(crc & 0xFFu);
    out[crc_at + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
    out[crc_at + 2] = static_cast<std::uint8_t>((crc >> 16) & 0xFFu);
    out[crc_at + 3] = static_cast<std::uint8_t>((crc >> 24) & 0xFFu);
    return out;
}

bool EditLog::peek_seed(std::span<const std::uint8_t> bytes,
                        std::uint32_t* out_seed) {
    if (bytes.size() < kEditLogHeaderBytes) return false;
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return false;
    if (bytes[4] != kEditLogVersion) return false;
    if (out_seed) *out_seed = get_u32(bytes.data() + 8);
    return true;
}

bool EditLog::decode(std::span<const std::uint8_t> bytes, EditLog& out,
                     std::uint32_t expected_seed) {
    out.records_.clear();
    if (bytes.size() < kEditLogHeaderBytes) return false;
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return false;
    if (bytes[4] != kEditLogVersion) return false;
    if (bytes[5] != 0 || bytes[6] != 0 || bytes[7] != 0) return false;

    const std::uint32_t seed = get_u32(bytes.data() + 8);
    if (seed != expected_seed) return false;

    const std::uint32_t count = get_u32(bytes.data() + 12);
    const std::uint32_t stored_crc = get_u32(bytes.data() + 16);
    // Checked before allocating: a corrupt count is the one field that can
    // turn a small file into a huge reserve.
    if (bytes.size() != kEditLogHeaderBytes +
                        static_cast<std::size_t>(count) * kRecordBytes) {
        return false;
    }

    std::vector<std::uint8_t> crc_input;
    crc_input.reserve(bytes.size());
    crc_input.insert(crc_input.end(), bytes.begin(), bytes.begin() + 16);
    crc_input.insert(crc_input.end(), bytes.begin() + kEditLogHeaderBytes,
                     bytes.end());
    if (crc32_ieee(crc_input.data(), crc_input.size()) != stored_crc) return false;

    out.records_.reserve(count);
    std::uint32_t last_tick = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* p = bytes.data() + kEditLogHeaderBytes
                              + static_cast<std::size_t>(i) * kRecordBytes;
        EditRecord r;
        r.tick = get_u32(p);
        r.x    = static_cast<std::int32_t>(get_u32(p + 4));
        r.z    = static_cast<std::int32_t>(get_u32(p + 8));
        r.y    = p[12];
        r.prev = p[13];
        r.next = p[14];
        r.reserved = 0;
        // The same rules record() enforces, applied to bytes off disk. A
        // log that passes the CRC can still be a log this build cannot
        // replay - out-of-order ticks or an unknown block id would seek to
        // a world that never existed rather than fail.
        if (r.tick == 0) return false;   // unseekable, as in record()
        if (r.tick < last_tick) return false;
        // No height check here, and that is deliberate rather than an
        // omission: y is a uint8_t and the world is exactly 256 blocks
        // tall, so every value the field can hold is a legal height. The
        // static_assert in the header is what keeps that true - widen the
        // world and this becomes a real check that has to be written.
        if (r.prev > kMaxBlockId || r.next > kMaxBlockId) return false;
        if (r.prev == r.next) return false;
        if (p[15] != 0) return false;
        last_tick = r.tick;
        out.records_.push_back(r);
    }
    return true;
}

}  // namespace world
