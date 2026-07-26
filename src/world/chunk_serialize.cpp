#include "world/chunk_serialize.h"

#include "world/block.h"

#include <array>
#include <cstring>

namespace world {

namespace {

constexpr char    kMagic[4]   = {'V', 'C', 'H', 'K'};
constexpr std::uint32_t kMaxRunLen = 0xFFFFu;  // u16 capacity

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

std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t len) {
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
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> encode_chunk_rle(const Chunk& c) {
    std::vector<std::uint8_t> out;
    out.reserve(kChunkFormatHeaderBytes + 64);

    out.insert(out.end(), kMagic, kMagic + 4);
    append_u8(out, kChunkFormatVersion);
    append_u8(out, 0);
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

    const std::uint32_t crc = crc32_ieee(out.data() + kChunkFormatHeaderBytes,
                                         out.size() - kChunkFormatHeaderBytes);
    out[8]  = static_cast<std::uint8_t>(crc & 0xFF);
    out[9]  = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    out[10] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
    out[11] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);
    return out;
}

bool decode_chunk_rle(const std::vector<std::uint8_t>& bytes, Chunk& out) {
    if (bytes.size() < kChunkFormatHeaderBytes) return false;
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return false;
    if (bytes[4] != kChunkFormatVersion) return false;
    // reserved bytes [5..7] ignored on read.

    // Integrity first: a payload that fails its CRC is corrupt regardless
    // of whether the damage happens to spell valid runs.
    const std::uint32_t stored = read_u32_le(bytes.data() + 8);
    const std::uint32_t actual =
        crc32_ieee(bytes.data() + kChunkFormatHeaderBytes,
                   bytes.size() - kChunkFormatHeaderBytes);
    if (stored != actual) return false;

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
    return true;
}

}  // namespace world
