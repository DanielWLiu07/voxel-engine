#pragma once

#include "world/chunk.h"

#include <cstdint>
#include <vector>

namespace world {

// Binary chunk file layout:
//   magic[4]      "VCHK"
//   version       u8  = 3
//   flags         u8  (bit 0: chunk holds player edits and must never be
//                 regenerated from terrain; other bits must be zero)
//   reserved      u16 (zero)
//   crc           u32 little-endian, CRC-32 (IEEE) of header bytes 0..7
//                 plus everything after the header - v3 folds the header
//                 under the CRC so a flipped edited bit cannot silently
//                 turn a hand-edited chunk back into a regenerable one
//   runs[]        repeated (u8 block_id, u16 run_length) until blocks sum
//                 to kChunkVolume
// Run lengths are little-endian and never zero; an all-of-one-block chunk
// is split into chunks of <= 65535 runs.
//
// The structural checks (known ids, exact volume, no trailing bytes) catch
// truncation and garbage; the CRC catches what they cannot - a flipped bit
// that still spells a valid run. Older versions are rejected, not
// migrated: saves are regenerable and the loader already counts and warns
// on skipped files.
constexpr std::uint8_t kChunkFormatVersion = 3;
constexpr std::size_t  kChunkFormatHeaderBytes = 12;

std::vector<std::uint8_t> encode_chunk_rle(const Chunk& c, bool edited = false);
// On success *edited (when non-null) reports the header's edited bit.
bool decode_chunk_rle(const std::vector<std::uint8_t>& bytes, Chunk& out,
                      bool* edited = nullptr);

// CRC-32 (IEEE 802.3, reflected 0xEDB88320). Exposed for tests.
std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t len);

}  // namespace world
