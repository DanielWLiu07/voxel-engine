#include "world/chunk_serialize.h"

#include "world/block.h"

#include <array>
#include <cstring>

namespace world {

namespace {

constexpr char    kMagic[4]   = {'V', 'C', 'H', 'K'};
constexpr std::uint32_t kMaxRunLen = 0xFFFFu;  // u16 capacity

// Header byte offsets, matching the layout comment in chunk_serialize.h.
constexpr std::size_t kOffsetVersion  = 4;
constexpr std::size_t kOffsetFlags    = 5;
constexpr std::size_t kOffsetReserved = 6;  // u16, bytes 6..7
constexpr std::size_t kOffsetCrc      = 8;  // u32, bytes 8..11

void append_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void emit_run(std::vector<std::uint8_t>& out, std::uint8_t id, std::uint32_t len) {
    // len fits caller-side; split is handled by encode_chunk_rle.
    append_u8(out, id);
    append_u16_le(out, static_cast<std::uint16_t>(len));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

namespace {

// Raw table-driven update over the pre-inverted running state, so the CRC
// can cover discontiguous spans (chunk_crc above stitches header and
// payload together).
std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data,
                           std::size_t len) {
    // Table built once, on first use; 256 u32s, thread-safe init.
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

std::uint32_t crc32_update(std::uint32_t, const std::uint8_t*, std::size_t);

// The format's CRC rule, stated once for encode and decode both: seed,
// run over header bytes 0..7 (magic, version, flags, reserved), skip the
// CRC field itself, continue over the payload, final-xor. If these two
// sides ever disagree, every save fails to load.
std::uint32_t chunk_crc(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t crc = crc32_update(0xFFFFFFFFu, bytes, kOffsetCrc);
    return crc32_update(crc, bytes + kChunkFormatHeaderBytes,
                        size - kChunkFormatHeaderBytes) ^ 0xFFFFFFFFu;
}

}  // namespace

std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t len) {
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> encode_chunk_rle(const Chunk& c, bool edited) {
    std::vector<std::uint8_t> out;
    out.reserve(kChunkFormatHeaderBytes + 64);

    out.insert(out.end(), kMagic, kMagic + 4);
    append_u8(out, kChunkFormatVersion);
    append_u8(out, edited ? 0x01 : 0x00);  // flags
    append_u16_le(out, 0);
    append_u32_le(out, 0);  // CRC placeholder, patched after the payload

    // Iterate in storage order (y outer, then z, then x) so runs match
    // the flat array layout exactly.
    std::uint8_t run_id = 0;
    std::uint32_t run_len = 0;
    bool have_run = false;

    auto flush = [&](std::uint8_t id, std::uint32_t len) {
        while (len > kMaxRunLen) {
            emit_run(out, id, kMaxRunLen);
            len -= kMaxRunLen;
        }
        if (len > 0) emit_run(out, id, len);
    };

    for (int y = 0; y < kChunkSizeY; ++y) {
        for (int z = 0; z < kChunkSizeZ; ++z) {
            for (int x = 0; x < kChunkSizeX; ++x) {
                std::uint8_t b = static_cast<std::uint8_t>(c.get(x, y, z));
                if (!have_run) { run_id = b; run_len = 1; have_run = true; }
                else if (b == run_id) { ++run_len; }
                else { flush(run_id, run_len); run_id = b; run_len = 1; }
            }
        }
    }
    if (have_run) flush(run_id, run_len);

    // v3: the CRC covers the header (version and flags - the edited bit
    // in particular - are integrity-checked too) plus the payload.
    const std::uint32_t crc = chunk_crc(out.data(), out.size());
    out[kOffsetCrc]     = static_cast<std::uint8_t>(crc & 0xFF);
    out[kOffsetCrc + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    out[kOffsetCrc + 2] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
    out[kOffsetCrc + 3] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);
    return out;
}

bool decode_chunk_rle(const std::vector<std::uint8_t>& bytes, Chunk& out,
                      bool* edited) {
    if (bytes.size() < kChunkFormatHeaderBytes) return false;
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return false;
    if (bytes[kOffsetVersion] != kChunkFormatVersion) return false;
    if ((bytes[kOffsetFlags] & ~0x01u) != 0) return false;  // unknown bits
    if (bytes[kOffsetReserved] != 0 || bytes[kOffsetReserved + 1] != 0)
        return false;

    // Integrity first: bytes that fail the CRC are corrupt regardless of
    // whether the damage happens to spell a valid header and runs.
    const std::uint32_t stored = read_u32_le(bytes.data() + kOffsetCrc);
    if (stored != chunk_crc(bytes.data(), bytes.size())) return false;

    Chunk decoded;
    std::size_t cursor = kChunkFormatHeaderBytes;
    std::uint32_t total = 0;
    while (cursor < bytes.size()) {
        if (cursor + 3 > bytes.size()) return false;
        std::uint8_t  id  = bytes[cursor];
        std::uint16_t len = static_cast<std::uint16_t>(
            bytes[cursor + 1] | (bytes[cursor + 2] << 8));
        cursor += 3;
        if (len == 0) return false;
        if (id > kMaxBlockId) return false;
        if (total + len > static_cast<std::uint32_t>(kChunkVolume)) return false;

        for (std::uint32_t k = 0; k < len; ++k) {
            std::uint32_t flat = total + k;
            int y = static_cast<int>(flat / (kChunkSizeZ * kChunkSizeX));
            int rem = static_cast<int>(flat % (kChunkSizeZ * kChunkSizeX));
            int z = rem / kChunkSizeX;
            int x = rem % kChunkSizeX;
            decoded.set(x, y, z, static_cast<BlockId>(id));
        }
        total += len;
    }

    if (total != static_cast<std::uint32_t>(kChunkVolume)) return false;
    if (cursor != bytes.size()) return false;
    out = std::move(decoded);
    if (edited) *edited = (bytes[kOffsetFlags] & 0x01u) != 0;
    return true;
}

}  // namespace world
