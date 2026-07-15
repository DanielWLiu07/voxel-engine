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
    return static_cast<bool>(f);
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

SaveStats save_world(const World& w, const std::string& dir) {
    SaveStats stats;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return stats;

    fs::path base = dir;
    bool any_error = false;
    w.for_each_chunk([&](ChunkCoord c, const Chunk& chunk) {
        if (any_error) return;
        auto bytes = encode_chunk_rle(chunk);
        fs::path path = base / chunk_filename(c);
        if (!write_bytes(path, bytes)) { any_error = true; return; }
        ++stats.chunks_written;
        stats.bytes_written += bytes.size();
        stats.bytes_raw     += static_cast<std::size_t>(kChunkVolume);
    });
    stats.ok = !any_error;
    return stats;
}

LoadStats load_world(World& w, const std::string& dir,
                     const TerrainGen& /*fallback_terrain*/,
                     core::ThreadPool& pool) {
    LoadStats stats;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return stats;

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
        if (!read_bytes(entry.path(), buf)) continue;

        Chunk chunk;
        if (!decode_chunk_rle(buf, chunk)) continue;

        stats.bytes_read += buf.size();
        stats.bytes_raw  += static_cast<std::size_t>(kChunkVolume);
        w.enqueue_decoded_chunk(coord, std::move(chunk), pool);
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
