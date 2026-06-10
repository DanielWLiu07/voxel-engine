#include "world/section_visibility.h"

#include "world/block.h"

#include <vector>

namespace world {

namespace {

constexpr int kCellsPerSection = kChunkSizeX * kSectionHeight * kChunkSizeZ;

constexpr int cell_index(int x, int ly, int z) {
    return (ly * kChunkSizeZ + z) * kChunkSizeX + x;
}

// Which faces a boundary cell touches, as a 6-bit mask.
constexpr std::uint8_t face_touch_mask(int x, int ly, int z) {
    std::uint8_t m = 0;
    if (x == 0)                   m |= 1u << kFaceNegX;
    if (x == kChunkSizeX - 1)     m |= 1u << kFacePosX;
    if (ly == 0)                  m |= 1u << kFaceNegY;
    if (ly == kSectionHeight - 1) m |= 1u << kFacePosY;
    if (z == 0)                   m |= 1u << kFaceNegZ;
    if (z == kChunkSizeZ - 1)     m |= 1u << kFacePosZ;
    return m;
}

constexpr SectionVisMask pairs_from_touch_mask(std::uint8_t faces) {
    SectionVisMask m = 0;
    for (int a = 0; a < 6; ++a) {
        if (!((faces >> a) & 1)) continue;
        for (int b = a + 1; b < 6; ++b) {
            if ((faces >> b) & 1) m |= SectionVisMask(1) << face_pair_bit(a, b);
        }
    }
    return m;
}

}  // namespace

SectionVisArray compute_section_visibility(const Chunk& chunk) {
    SectionVisArray out{};

    // Fully empty chunk: every section is wide open, skip the fill.
    if (chunk.empty()) {
        out.fill(kSectionVisAll);
        return out;
    }

    std::vector<std::uint8_t> visited(kCellsPerSection);
    std::vector<int> stack;
    stack.reserve(kCellsPerSection);

    for (int sy = 0; sy < kSectionsPerChunk; ++sy) {
        const int y0 = sy * kSectionHeight;
        std::fill(visited.begin(), visited.end(), 0);
        SectionVisMask mask = 0;

        for (int ly = 0; ly < kSectionHeight; ++ly) {
            for (int z = 0; z < kChunkSizeZ; ++z) {
                for (int x = 0; x < kChunkSizeX; ++x) {
                    const int idx = cell_index(x, ly, z);
                    if (visited[idx]) continue;
                    if (is_solid(chunk.get(x, y0 + ly, z))) continue;

                    // New air component: DFS, accumulate touched faces.
                    std::uint8_t faces = 0;
                    visited[idx] = 1;
                    stack.push_back(idx);
                    while (!stack.empty()) {
                        const int cur = stack.back();
                        stack.pop_back();
                        const int cx = cur % kChunkSizeX;
                        const int cz = (cur / kChunkSizeX) % kChunkSizeZ;
                        const int cy = cur / (kChunkSizeX * kChunkSizeZ);
                        faces |= face_touch_mask(cx, cy, cz);

                        auto try_cell = [&](int nx, int ny, int nz) {
                            if (nx < 0 || nx >= kChunkSizeX) return;
                            if (ny < 0 || ny >= kSectionHeight) return;
                            if (nz < 0 || nz >= kChunkSizeZ) return;
                            const int ni = cell_index(nx, ny, nz);
                            if (visited[ni]) return;
                            if (is_solid(chunk.get(nx, y0 + ny, nz))) return;
                            visited[ni] = 1;
                            stack.push_back(ni);
                        };
                        try_cell(cx - 1, cy, cz);
                        try_cell(cx + 1, cy, cz);
                        try_cell(cx, cy - 1, cz);
                        try_cell(cx, cy + 1, cz);
                        try_cell(cx, cy, cz - 1);
                        try_cell(cx, cy, cz + 1);
                    }
                    mask |= pairs_from_touch_mask(faces);
                    if (mask == kSectionVisAll) goto section_done;
                }
            }
        }
    section_done:
        out[sy] = mask;
    }
    return out;
}

}  // namespace world
