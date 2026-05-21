#pragma once

#include "world/chunk.h"

#include <cstdint>
#include <vector>

namespace world {

// Binary chunk file layout:
//   magic[4]      "VCHK"
//   version       u8  = 1
//   reserved      u8, u16 (both zero)
//   runs[]        repeated (u8 block_id, u16 run_length) until blocks sum
//                 to kChunkVolume
// Run lengths are little-endian and never zero; an all-of-one-block chunk
// is split into chunks of <= 65535 runs.
constexpr std::uint8_t kChunkFormatVersion = 1;
constexpr std::size_t  kChunkFormatHeaderBytes = 8;

std::vector<std::uint8_t> encode_chunk_rle(const Chunk& c);
bool decode_chunk_rle(const std::vector<std::uint8_t>& bytes, Chunk& out);

}  // namespace world
