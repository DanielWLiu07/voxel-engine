#pragma once

#include "world/chunk.h"

#include <array>
#include <cstdint>

namespace world {

// Face indices for section connectivity. Opposite faces pair up via f ^ 1.
enum SectionFace : int {
    kFaceNegX = 0, kFacePosX = 1,
    kFaceNegY = 2, kFacePosY = 3,
    kFaceNegZ = 4, kFacePosZ = 5,
};

constexpr int opposite_face(int f) { return f ^ 1; }

// 15-bit mask over the C(6,2) unordered face pairs of one section. Bit set
// means a sightline can pass through the section's air between those two
// faces (same flood-fill component touches both). A fully solid section is
// 0; a fully empty one is 0x7FFF.
using SectionVisMask = std::uint16_t;
using SectionVisArray = std::array<SectionVisMask, kSectionsPerChunk>;

inline constexpr SectionVisMask kSectionVisAll = 0x7FFF;

constexpr int face_pair_bit(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    // offset(a) = number of pairs (a', b') with a' < a.
    return (5 * a - a * (a - 1) / 2) + (b - a - 1);
}

constexpr bool faces_connected(SectionVisMask m, int a, int b) {
    return a == b || (m >> face_pair_bit(a, b)) & 1;
}

// Flood-fills the air cells of each 16x32x16 section and records which face
// pairs each connected component bridges. CPU-only, no GL — runs on the
// worker thread next to the greedy mesher (~8k cells/section).
SectionVisArray compute_section_visibility(const Chunk& chunk);

}  // namespace world
