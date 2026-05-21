#pragma once

#include "world/terrain_gen.h"
#include "world/world.h"

#include <cstddef>
#include <string>

namespace world {

struct SaveStats {
    int         chunks_written = 0;
    std::size_t bytes_written  = 0;   // compressed (on-disk)
    std::size_t bytes_raw      = 0;   // uncompressed (kChunkVolume * chunks)
    bool        ok             = false;
};

struct LoadStats {
    int         chunks_read = 0;
    std::size_t bytes_read  = 0;
    std::size_t bytes_raw   = 0;
    bool        ok          = false;
};

// Writes one file per loaded chunk to "<dir>/chunk_<x>_<z>.vchk".
SaveStats save_world(const World& w, const std::string& dir);

// Decodes every "chunk_<x>_<z>.vchk" file in dir into the World. Caller is
// expected to clear_all() first if they want a fresh state; this routine
// just inserts what it finds and ignores everything else. The terrain
// reference is unused on the success path and reserved for future fallback.
LoadStats load_world(World& w, const std::string& dir,
                     const TerrainGen& fallback_terrain);

}  // namespace world
