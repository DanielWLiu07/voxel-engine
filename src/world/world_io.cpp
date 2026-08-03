#include "world/world_io.h"

#include "world/chunk_serialize.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace world {

namespace {

std::string chunk_filename(ChunkCoord c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "chunk_%d_%d.vchk", c.x, c.z);
    return std::string(buf);
}

bool parse_int(std::string_view s, int& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end   = s.data() + s.size();
    auto r = std::from_chars(begin, end, out);
    return r.ec == std::errc() && r.ptr == end;
}

// Parses "chunk_<x>_<z>.vchk"; signed ints, negatives allowed.
bool parse_chunk_filename(std::string_view name, ChunkCoord& out) {
    static const std::string_view prefix = "chunk_";
    static const std::string_view suffix = ".vchk";
    if (name.size() < prefix.size() + suffix.size() + 3) return false;
    if (name.substr(0, prefix.size()) != prefix)                            return false;
    if (name.substr(name.size() - suffix.size()) != suffix)                 return false;

    std::string_view mid = name.substr(
        prefix.size(), name.size() - prefix.size() - suffix.size());
    // First '_' separator skips a possible leading minus on x.
    auto under = mid.find('_', 1);
    if (under == std::string_view::npos || under == 0
        || under == mid.size() - 1) return false;

    int x = 0, z = 0;
    if (!parse_int(mid.substr(0, under), x))            return false;
    if (!parse_int(mid.substr(under + 1), z))           return false;
    out = {x, z};
    return true;
}

bool write_bytes(const fs::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    // Close before judging success: filebuf buffers more than most RLE
    // chunks, so stream state before the flush-on-close proves nothing
    // reached the OS - and the destructor's implicit flush swallows
    // errors (ENOSPC, I/O failure). An unverified .tmp here is exactly
    // the torn file the atomic rename exists to prevent.
    f.close();
    return !f.fail();
}

bool read_bytes(const fs::path& path, std::vector<std::uint8_t>& out) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return false;
    out.resize(sz);
    if (sz == 0) return true;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(sz));
    return static_cast<bool>(f);
}

}  // namespace

bool write_bytes_atomic(const fs::path& path,
                        const std::vector<std::uint8_t>& bytes) {
    fs::path tmp = path;
    tmp += ".tmp";
    if (!write_bytes(tmp, bytes)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        return false;
    }
    return true;
}

bool write_world_manifest(const std::string& dir, std::uint32_t seed) {
    const std::string line = "seed " + std::to_string(seed) + "\n";
    return write_bytes_atomic(fs::path(dir) / "world.manifest",
                              {line.begin(), line.end()});
}

bool read_world_manifest(const std::string& dir, std::uint32_t& seed_out) {
    std::vector<std::uint8_t> buf;
    if (!read_bytes(fs::path(dir) / "world.manifest", buf)) return false;
    const std::string text(buf.begin(), buf.end());
    if (text.rfind("seed ", 0) != 0) return false;
    unsigned long long v = 0;
    std::size_t used = 0;
    try { v = std::stoull(text.substr(5), &used); } catch (...) { return false; }
    if (v > 0xFFFFFFFFull) return false;
    seed_out = static_cast<std::uint32_t>(v);
    return true;
}

SaveStats save_world(const World& w, const std::string& dir,
                     std::uint32_t terrain_seed) {
    SaveStats stats;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return stats;

    fs::path base = dir;
    bool any_error = !write_world_manifest(dir, terrain_seed);
    w.for_each_chunk([&](ChunkCoord c, const Chunk& chunk) {
        if (any_error) return;
        // The edited bit rides in the chunk header so a load without this
        // session's memory still knows which chunks it must preserve.
        auto bytes = encode_chunk_rle(chunk, w.chunk_is_preserved(c));
        fs::path path = base / chunk_filename(c);
        if (!write_bytes_atomic(path, bytes)) { any_error = true; return; }
        ++stats.chunks_written;
        stats.bytes_written += bytes.size();
        stats.bytes_raw     += static_cast<std::size_t>(kChunkVolume);
    });
    // Edited chunks the stream window evicted exist only as the RLE stash;
    // a save that skipped them would drop every edit made out of view. A
    // coord both stashed and resident saves the resident copy (newer).
    w.for_each_stashed([&](ChunkCoord c, const std::vector<std::uint8_t>& bytes) {
        if (any_error || w.has_chunk(c)) return;
        fs::path path = base / chunk_filename(c);
        if (!write_bytes_atomic(path, bytes)) { any_error = true; return; }
        ++stats.chunks_written;
        stats.bytes_written += bytes.size();
        stats.bytes_raw     += static_cast<std::size_t>(kChunkVolume);
    });
    stats.ok = !any_error;
    return stats;
}

LoadStats load_world(World& w, const std::string& dir,
                     const TerrainGen& /*fallback_terrain*/,
                     core::ThreadPool& pool,
                     std::uint32_t active_seed) {
    LoadStats stats;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return stats;

    // Without a manifest whose seed matches the active terrain, nothing
    // on disk is regenerable and every chunk loads preserve-marked (the
    // conservative pre-manifest behavior). With a match, only chunks
    // whose header carries the edited bit are preserved; the rest can be
    // dropped on eviction and regenerated on re-entry.
    std::uint32_t manifest_seed = 0;
    stats.seed_matched = read_world_manifest(dir, manifest_seed) &&
                         manifest_seed == active_seed;

    fs::directory_iterator it(dir, ec);
    if (ec) return stats;

    // Read + RLE-decode each file on the calling thread (fast - the
    // decompression payload at 144x ratio is ~270 KB total for radius 12)
    // then hand the chunk to the worker pool for greedy meshing. The pool
    // pushes results onto the world's finished queue; we drain that on the
    // caller thread because the GL upload that happens in build_slot has
    // to run on the GL-owning thread.
    std::vector<std::uint8_t> buf;
    int enqueued = 0;
    for (const auto& entry : it) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        ChunkCoord coord{};
        if (!parse_chunk_filename(name, coord)) continue;

        buf.clear();
        if (!read_bytes(entry.path(), buf)) { ++stats.files_skipped; continue; }

        Chunk chunk;
        bool edited = false;
        if (!decode_chunk_rle(buf, chunk, &edited)) { ++stats.files_skipped; continue; }

        stats.bytes_read += buf.size();
        stats.bytes_raw  += static_cast<std::size_t>(kChunkVolume);
        w.enqueue_decoded_chunk(coord, std::move(chunk), pool,
                                edited || !stats.seed_matched);
        ++enqueued;
    }

    // Drain until every enqueued chunk has its mesh and slot built.
    while (w.pending_async() > 0) {
        const int got = w.drain_finished(64);
        if (got == 0) std::this_thread::yield();
    }
    stats.chunks_read = enqueued;
    stats.ok = true;
    return stats;
}

}  // namespace world
