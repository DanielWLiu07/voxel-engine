#pragma once

#include "core/thread_pool.h"
#include "world/terrain_gen.h"
#include "world/world.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace world {

// Atomically replaces `path` with `bytes`: writes "<path>.tmp" in the same
// directory, then renames it over the target. A crash mid-save therefore
// leaves either the previous file or a stray .tmp (which the loader's
// filename parse ignores), never a torn half-written .vchk. This guards
// against torn writes, not power loss - there is no fsync before the
// rename. On failure the target is untouched and the .tmp is removed.
bool write_bytes_atomic(const std::filesystem::path& path,
                        const std::vector<std::uint8_t>& bytes);

struct SaveStats {
    int         chunks_written = 0;
    std::size_t bytes_written  = 0;   // compressed (on-disk)
    std::size_t bytes_raw      = 0;   // uncompressed (kChunkVolume * chunks)
    bool        ok             = false;
};

struct LoadStats {
    int         chunks_read = 0;
    // .vchk files present but unreadable or failing RLE decode: corruption,
    // never silently dropped. Non-chunk filenames are simply ignored.
    int         files_skipped = 0;
    std::size_t bytes_read  = 0;
    std::size_t bytes_raw   = 0;
    bool        ok          = false;
};

// Writes one file per loaded chunk to "<dir>/chunk_<x>_<z>.vchk".
SaveStats save_world(const World& w, const std::string& dir);

// Decodes every "chunk_<x>_<z>.vchk" file in dir into the World. Caller is
// expected to clear_all() first if they want a fresh state; this routine
// just inserts what it finds and ignores everything else.
//
// Greedy meshing happens on the worker pool in parallel; the caller's
// thread (which must own the GL context) drains the finished queue and
// performs the GL upload. Blocks until all loaded chunks have been
// inserted into the world. The terrain reference is reserved for future
// fallback when a saved chunk is missing.
LoadStats load_world(World& w, const std::string& dir,
                     const TerrainGen& fallback_terrain,
                     core::ThreadPool& pool);

}  // namespace world
