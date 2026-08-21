#include "world/world.h"

#include "core/profiler.h"
#include "world/chunk_mesh.h"
#include "world/chunk_serialize.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

constexpr int floor_div(int a, int n) {
    int q = a / n;
    int r = a % n;
    if ((r != 0) && ((r < 0) != (n < 0))) --q;
    return q;
}

constexpr int floor_mod(int a, int n) {
    int r = a % n;
    if (r < 0) r += n;
    return r;
}

}  // namespace

namespace world {

namespace {

// Min/max Y of any solid block in the chunk. Returned as a closed range
// (max_y is inclusive). Empty chunks return {0,0} - slot has no mesh so
// the AABB is never tested anyway.
std::pair<int,int> tight_y_range(const Chunk& chunk) {
    int min_y = -1, max_y = -1;
    for (int y = 0; y < kChunkSizeY; ++y) {
        bool any_solid = false;
        for (int z = 0; z < kChunkSizeZ && !any_solid; ++z) {
            for (int x = 0; x < kChunkSizeX; ++x) {
                if (is_solid(chunk.get(x, y, z))) { any_solid = true; break; }
            }
        }
        if (any_solid) {
            if (min_y < 0) min_y = y;
            max_y = y;
        }
    }
    if (min_y < 0) return {0, 0};
    return {min_y, max_y};
}

}  // namespace

// Tight per-chunk AABB derived from the chunk's solid extents. With wide
// (0..256) Y, a horizontally-forward frustum intersects almost every column
// it overlaps in XZ - vertical pruning collapses to nothing. Tight Y means
// chunks whose terrain sits well above or below the camera get culled.
gfx::AABB make_chunk_aabb(ChunkCoord c, const Chunk& chunk) {
    const float ox = static_cast<float>(c.x * kChunkSizeX);
    const float oz = static_cast<float>(c.z * kChunkSizeZ);
    auto [min_y, max_y] = tight_y_range(chunk);
    return {{ox,                 static_cast<float>(min_y),    oz},
            {ox + kChunkSizeX,   static_cast<float>(max_y + 1), oz + kChunkSizeZ}};
}

namespace {

// One section's mesh in-build form. Vertices are the same chunk-LOCAL
// positions the mesher emits; the world-space AABB folds in the chunk
// origin so cull tests don't need the model matrix.
struct SectionBuild {
    std::vector<gfx::VertexPacked> vertices;
    glm::vec3 aabb_min{};
    glm::vec3 aabb_max{};
    int  quad_count   = 0;
    bool initialized  = false;
};

// Bucket the chunk-wide greedy mesh into per-section slices. Each quad is
// assigned to the section that contains its bottom Y; the section's AABB
// extends to the quad's actual extent, so a side face that spans two
// sections still draws correctly when the higher section is in-frustum
// (the AABB pulls the lower section in too - conservative, correct).
//
// Bucketing instead of meshing-per-section keeps the greedy merger
// chunk-wide, so we don't lose face runs at section boundaries - bullet
// #1 (greedy ratio) doesn't regress.
std::array<SectionBuild, kSectionsPerChunk>
bucket_quads_by_section(const ChunkMeshData& src, ChunkCoord coord) {
    std::array<SectionBuild, kSectionsPerChunk> out;
    const float ox = static_cast<float>(coord.x * kChunkSizeX);
    const float oz = static_cast<float>(coord.z * kChunkSizeZ);

    const std::size_t quad_count = src.vertices.size() / 4;
    for (std::size_t q = 0; q < quad_count; ++q) {
        const glm::vec3 p0 = src.vertices[4 * q + 0].pos();
        const glm::vec3 p1 = src.vertices[4 * q + 1].pos();
        const glm::vec3 p2 = src.vertices[4 * q + 2].pos();
        const glm::vec3 p3 = src.vertices[4 * q + 3].pos();

        const float ymin = std::min({p0.y, p1.y, p2.y, p3.y});
        const float ymax = std::max({p0.y, p1.y, p2.y, p3.y});
        const float xmin = std::min({p0.x, p1.x, p2.x, p3.x});
        const float xmax = std::max({p0.x, p1.x, p2.x, p3.x});
        const float zmin = std::min({p0.z, p1.z, p2.z, p3.z});
        const float zmax = std::max({p0.z, p1.z, p2.z, p3.z});

        const int section_idx = std::clamp(
            static_cast<int>(std::floor(ymin)) / kSectionHeight,
            0, kSectionsPerChunk - 1);
        SectionBuild& s = out[section_idx];

        // Copy the quad's four vertices as emitted: the mesher encodes the
        // AO diagonal flip (ao_flip) in the vertex order itself, so a plain
        // copy preserves it and the shared quad index pattern applies to
        // every quad uniformly.
        s.vertices.push_back(src.vertices[4 * q + 0]);
        s.vertices.push_back(src.vertices[4 * q + 1]);
        s.vertices.push_back(src.vertices[4 * q + 2]);
        s.vertices.push_back(src.vertices[4 * q + 3]);
        ++s.quad_count;

        const glm::vec3 lo{xmin + ox, ymin, zmin + oz};
        const glm::vec3 hi{xmax + ox, ymax, zmax + oz};
        if (!s.initialized) {
            s.aabb_min = lo;
            s.aabb_max = hi;
            s.initialized = true;
        } else {
            s.aabb_min = glm::min(s.aabb_min, lo);
            s.aabb_max = glm::max(s.aabb_max, hi);
        }
    }
    return out;
}

// Concatenate all non-empty section meshes into one vertex buffer per
// chunk and upload once. Section quads stay contiguous, so each section
// keeps an (index_offset, index_count) slice into the engine-wide shared
// quad index pattern: culling and drawing happen at section granularity,
// but the GL upload + VAO bind happen at chunk granularity, and no index
// data is built or uploaded per chunk at all.
void apply_sections(ChunkSlot& slot,
                    std::array<SectionBuild, kSectionsPerChunk>&& built,
                    gfx::QuadIndexBuffer& quad_indices) {
    slot.any_section_has_mesh = false;
    bool union_init = false;

    std::vector<gfx::VertexPacked> all_vertices;
    std::size_t reserved_v = 0;
    for (const auto& s : built) reserved_v += s.vertices.size();
    all_vertices.reserve(reserved_v);

    for (int i = 0; i < kSectionsPerChunk; ++i) {
        auto& dst = slot.sections[i];
        auto& src = built[i];
        if (src.quad_count == 0) {
            dst.has_mesh     = false;
            dst.index_offset = 0;
            dst.index_count  = 0;
            continue;
        }
        // Quad q of the chunk buffer draws with indices [6q, 6q+6) of the
        // shared pattern, so a section's slice is just its quad range
        // scaled by 6.
        const std::uint32_t quad_base =
            static_cast<std::uint32_t>(all_vertices.size() / 4);
        all_vertices.insert(all_vertices.end(), src.vertices.begin(), src.vertices.end());

        dst.aabb         = {src.aabb_min, src.aabb_max};
        dst.index_offset = 6 * quad_base;
        dst.index_count  = static_cast<std::uint32_t>(6 * src.quad_count);
        dst.has_mesh     = true;
        slot.any_section_has_mesh = true;

        if (!union_init) {
            slot.chunk_aabb = dst.aabb;
            union_init = true;
        } else {
            slot.chunk_aabb.min = glm::min(slot.chunk_aabb.min, dst.aabb.min);
            slot.chunk_aabb.max = glm::max(slot.chunk_aabb.max, dst.aabb.max);
        }
    }
    if (slot.any_section_has_mesh) {
        slot.chunk_mesh.upload(all_vertices, quad_indices);
    }
    // Vertex bytes only: the shared quad pattern is one buffer engine-wide,
    // counted once in resident_gpu_bytes().
    slot.gpu_bytes = all_vertices.size() * sizeof(gfx::VertexPacked);
    if (!union_init) {
        // Fully empty chunk - fall back to the block-extent AABB (returns a
        // zero-extent box; nothing will pass the cull test).
        slot.chunk_aabb = make_chunk_aabb(slot.coord, slot.chunk);
    }
}

}  // namespace

std::array<SectionBounds, kSectionsPerChunk>
compute_section_bounds(ChunkCoord coord, const Chunk& chunk) {
    auto mesh_data = build_chunk_mesh_greedy(chunk);
    auto built     = bucket_quads_by_section(mesh_data, coord);
    std::array<SectionBounds, kSectionsPerChunk> out;
    for (int i = 0; i < kSectionsPerChunk; ++i) {
        if (built[i].initialized) {
            out[i].aabb     = {built[i].aabb_min, built[i].aabb_max};
            out[i].has_mesh = true;
        }
    }
    return out;
}

namespace {

std::unique_ptr<ChunkSlot> build_slot(ChunkCoord coord, Chunk&& chunk,
                                      ChunkMeshData&& mesh_data,
                                      const SectionVisArray& visibility,
                                      gfx::QuadIndexBuffer& quad_indices) {
    auto slot = std::make_unique<ChunkSlot>();
    slot->coord = coord;
    slot->chunk = std::move(chunk);
    slot->section_visibility = visibility;
    auto built = bucket_quads_by_section(mesh_data, coord);
    apply_sections(*slot, std::move(built), quad_indices);
    return slot;
}

}  // namespace

void World::generate_grid(int radius, const ColumnFiller& fill_column) {
    chunks_.clear();
    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            Chunk chunk;
            const int origin_x = cx * kChunkSizeX;
            const int origin_z = cz * kChunkSizeZ;
            for (int z = 0; z < kChunkSizeZ; ++z) {
                for (int x = 0; x < kChunkSizeX; ++x) {
                    fill_column(origin_x + x, origin_z + z, chunk, x, z);
                }
            }
            auto mesh_data = build_chunk_mesh(mesher_kind_, chunk);
            auto vis = compute_section_visibility(chunk);
            ChunkCoord c{cx, cz};
            chunks_.emplace(c, build_slot(c, std::move(chunk), std::move(mesh_data), vis, quad_ibo_));
        }
    }
}

World::GenStats World::generate_grid(int radius, const TerrainGen& terrain) {
    using clock = std::chrono::steady_clock;
    GenStats stats;
    auto wall_t0 = clock::now();

    chunks_.clear();
    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            Chunk chunk;
            auto gen_t0 = clock::now();
            terrain.fill_chunk(cx, cz, chunk);
            stats.gen_ms += std::chrono::duration<double, std::milli>(clock::now() - gen_t0).count();

            auto mesh_t0 = clock::now();
            auto mesh_data = build_chunk_mesh(mesher_kind_, chunk);
            stats.mesh_ms += std::chrono::duration<double, std::milli>(clock::now() - mesh_t0).count();

            auto vis = compute_section_visibility(chunk);
            ChunkCoord c{cx, cz};
            chunks_.emplace(c, build_slot(c, std::move(chunk), std::move(mesh_data), vis, quad_ibo_));
            ++stats.chunks_generated;
        }
    }

    stats.total_ms = std::chrono::duration<double, std::milli>(clock::now() - wall_t0).count();
    return stats;
}

// Registers an outstanding request for c (stamping it so a stale in-flight
// job can never satisfy a newer request) and submits the terrain+mesh job.
// The one place the worker-side pipeline is spelled out; the grid preload
// and the streaming window both go through it.
void World::request_terrain_chunk(ChunkCoord c, const TerrainGen& terrain,
                                  core::ThreadPool& pool) {
    const std::uint64_t stamp = ++request_seq_;
    const std::uint64_t gen = generation_;
    requested_[c] = stamp;
    jobs_in_flight_.fetch_add(1);
    std::uint8_t mask = 0;
    NeighborPlanes planes = neighbor_planes_for(c, &mask);
    const MesherKind kind = mesher_kind_;
    NeighborLight nlight = neighbor_light_for(c);
    pool.submit([this, &terrain, c, gen, stamp, mask, kind,
                 planes = std::move(planes),
                 nlight = std::move(nlight)]() {
        ZoneScopedN("chunk_worker_job");
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        FinishedChunk fc;
        fc.coord = c;
        fc.generation = gen;
        fc.request_stamp = stamp;
        terrain.fill_chunk(c.x, c.z, fc.chunk);
        const auto t_after_terrain = clock::now();
        fc.terrain_ms = std::chrono::duration<double, std::milli>(
            t_after_terrain - t0).count();
        propagate_light(fc.chunk, nlight, fc.light);
        fc.mesh_data = build_chunk_mesh(kind, fc.chunk, planes,
                                        {&fc.light, &nlight});
        fc.neighbor_mask = mask;
        fc.visibility = compute_section_visibility(fc.chunk);
        fc.worker_ms = std::chrono::duration<double, std::milli>(
            clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(finished_mutex_);
        finished_.push(std::move(fc));
    });
}

void World::enqueue_grid_async(int radius, const TerrainGen& terrain,
                               core::ThreadPool& pool) {
    chunks_.clear();
    requested_.clear();
    {
        std::lock_guard<std::mutex> lock(finished_mutex_);
        std::queue<FinishedChunk> empty;
        finished_.swap(empty);
    }
    jobs_in_flight_.store(0);
    ++generation_;  // any job from a previous grid is now stale

    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            request_terrain_chunk({cx, cz}, terrain, pool);
        }
    }
}

World::StreamStats World::update_streaming(ChunkCoord center, int radius,
                                           const TerrainGen& terrain,
                                           core::ThreadPool& pool) {
    StreamStats stats;
    auto in_window = [&](ChunkCoord c) {
        return std::abs(c.x - center.x) <= radius
            && std::abs(c.z - center.z) <= radius;
    };

    for (auto it = chunks_.begin(); it != chunks_.end(); ) {
        if (!in_window(it->first)) {
            // Edits must survive eviction: regeneration from the terrain
            // generator would silently undo them. Unmodified chunks are
            // cheaper to regenerate than to keep.
            if (it->second->player_modified) {
                edited_stash_[it->first] = encode_chunk_rle(it->second->chunk, /*edited=*/true);
                ++stats.stashed;
            }
            it = chunks_.erase(it);
            ++stats.evicted;
        }
        else ++it;
    }
    // In-flight jobs that fell out of window stay scheduled but their
    // results get dropped in drain_finished.
    for (auto it = requested_.begin(); it != requested_.end(); ) {
        if (!in_window(it->first)) it = requested_.erase(it);
        else ++it;
    }

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            ChunkCoord c{center.x + dx, center.z + dz};
            if (chunks_.count(c) || requested_.count(c)) continue;
            // A stashed edit takes priority over fresh terrain. Decode is
            // main-thread (microseconds at RLE sizes); meshing still goes
            // through the worker pool like any load.
            if (auto sit = edited_stash_.find(c); sit != edited_stash_.end()) {
                Chunk restored;
                if (decode_chunk_rle(sit->second, restored)) {
                    enqueue_decoded_chunk(c, std::move(restored), pool,
                                          /*preserve_on_evict=*/true);
                    ++stats.restored;
                    continue;
                }
                // A stash entry we wrote ourselves failing to decode is a
                // bug, not a file-corruption case; fall through to terrain
                // rather than crash-loop on it.
            }
            request_terrain_chunk(c, terrain, pool);
            ++stats.requested;
        }
    }
    return stats;
}

int World::drain_finished(int max_per_frame) {
    ZoneScopedN("drain_finished");
    using clock = std::chrono::steady_clock;
    int uploaded = 0;
    for (int i = 0; i < max_per_frame; ++i) {
        FinishedChunk fc;
        {
            std::lock_guard<std::mutex> lock(finished_mutex_);
            if (finished_.empty()) break;
            fc = std::move(finished_.front());
            finished_.pop();
        }

        jobs_in_flight_.fetch_sub(1);
        total_worker_ms_  += fc.worker_ms;
        total_terrain_ms_ += fc.terrain_ms;
        total_mesh_ms_    += fc.mesh_data.build_ms;

        // A result from before the last wipe would drop regenerated terrain
        // over a freshly loaded world; discard it (its slot in requested_ is
        // gone or belongs to the current generation's job).
        if (fc.generation != generation_) continue;
        auto req_it = requested_.find(fc.coord);
        if (req_it == requested_.end()) continue;  // evicted mid-flight
        // Only the job answering the OUTSTANDING request may land. A coord
        // cycles through request/evict/re-request while its first job sits
        // in the pool backlog; accepting any coord match would let stale
        // pristine terrain drain over a newer stash restore and unmark the
        // slot, silently reverting the player's edit.
        if (fc.request_stamp != req_it->second) continue;
        requested_.erase(req_it);

        const auto up_t0 = clock::now();
        const std::uint8_t landed_mask = fc.neighbor_mask;
        auto new_slot = build_slot(fc.coord, std::move(fc.chunk),
                                   std::move(fc.mesh_data), fc.visibility,
                                   quad_ibo_);
        // find-then-assign rather than emplace: emplace is free to move
        // from its argument even when the key already exists, and the
        // re-mesh path below needs the slot intact in exactly that case.
        auto slot_it = chunks_.find(fc.coord);
        if (slot_it == chunks_.end()) {
            // Chunks that came from disk or the edit stash must keep
            // stashing on eviction; the terrain generator cannot reproduce
            // them.
            new_slot->player_modified = fc.preserve_on_evict;
            slot_it = chunks_.emplace(fc.coord, std::move(new_slot)).first;
        } else {
            // A re-mesh of a chunk that is already resident. Assigning the
            // slot swaps in the new GL mesh and destroys the old one here
            // on the main thread, where deleting GL objects is legal.
            new_slot->player_modified = slot_it->second->player_modified ||
                                        fc.preserve_on_evict;
            slot_it->second = std::move(new_slot);
        }
        slot_it->second->meshed_with = landed_mask;
        slot_it->second->light = fc.light;
        // Anything already resident beside this chunk was meshed without
        // it and is still drawing the faces it now hides.
        mark_neighbors_dirty(fc.coord);
        total_upload_ms_ += std::chrono::duration<double, std::milli>(
            clock::now() - up_t0).count();
        ++uploaded;
    }
    return uploaded;
}

NeighborLight World::neighbor_light_for(ChunkCoord c) const {
    NeighborLight out;
    auto copy = [&](LightPlane& p, int dx, int dz, bool along_z, int fixed) {
        auto it = chunks_.find({c.x + dx, c.z + dz});
        if (it == chunks_.end()) return;
        p.present = true;
        const LightGrid& g = it->second->light;
        for (int y = 0; y < kChunkSizeY; ++y) {
            for (int t = 0; t < kChunkSizeX; ++t) {
                p.set(t, y, along_z ? g.get(fixed, y, t) : g.get(t, y, fixed));
            }
        }
    };
    copy(out.neg_x, -1, 0, true,  kChunkSizeX - 1);
    copy(out.pos_x,  1, 0, true,  0);
    copy(out.neg_z, 0, -1, false, kChunkSizeZ - 1);
    copy(out.pos_z, 0,  1, false, 0);
    return out;
}

NeighborPlanes World::neighbor_planes_for(ChunkCoord c,
                                          std::uint8_t* out_mask) const {
    const auto at = [&](int dx, int dz) -> const Chunk* {
        auto it = chunks_.find({c.x + dx, c.z + dz});
        return it == chunks_.end() ? nullptr : &it->second->chunk;
    };
    const NeighborChunks n{.neg_x = at(-1, 0), .pos_x = at(1, 0),
                           .neg_z = at(0, -1), .pos_z = at(0, 1)};
    if (out_mask != nullptr) {
        std::uint8_t m = 0;
        if (n.neg_x) m |= kNeighborNegX;
        if (n.pos_x) m |= kNeighborPosX;
        if (n.neg_z) m |= kNeighborNegZ;
        if (n.pos_z) m |= kNeighborPosZ;
        *out_mask = m;
    }
    return NeighborPlanes::from(n);
}

// A chunk that landed after its neighbours were meshed leaves those
// neighbours holding boundary faces it now hides. Rather than re-meshing
// them immediately - which would multiply the work of a bulk load by five
// and spike the frame that happens to drain the last chunk - they are
// queued and drained a few at a time.
void World::mark_neighbors_dirty(ChunkCoord c) {
    const std::pair<ChunkCoord, std::uint8_t> sides[4] = {
        {{c.x - 1, c.z}, kNeighborPosX},   // our -X neighbour sees us at +X
        {{c.x + 1, c.z}, kNeighborNegX},
        {{c.x, c.z - 1}, kNeighborPosZ},
        {{c.x, c.z + 1}, kNeighborNegZ},
    };
    for (const auto& [coord, bit] : sides) {
        auto it = chunks_.find(coord);
        if (it == chunks_.end()) continue;
        // Already meshed against us: nothing changed for it.
        if (it->second->meshed_with & bit) continue;
        if (dirty_set_.insert(coord).second) dirty_meshes_.push_back(coord);
    }
}

// Queues a resident chunk for a re-mesh. Used when something outside it
// changed in a way that alters which of its faces are hidden.
void World::queue_remesh(ChunkCoord c) {
    if (chunks_.find(c) == chunks_.end()) return;
    if (dirty_set_.insert(c).second) dirty_meshes_.push_back(c);
}

int World::flush_pending_remeshes(core::ThreadPool& pool, int max_jobs) {
    int issued = 0;
    while (issued < max_jobs && !dirty_meshes_.empty()) {
        const ChunkCoord c = dirty_meshes_.back();
        dirty_meshes_.pop_back();
        dirty_set_.erase(c);

        auto it = chunks_.find(c);
        if (it == chunks_.end()) continue;         // evicted since marking
        if (requested_.count(c) != 0) continue;    // a job is already coming

        // Hand the worker a copy of the chunk and of the boundary layers.
        // Copying rather than pointing is the whole reason this is safe to
        // run off-thread: the main thread stays free to edit or evict any
        // of these chunks while the job is in flight.
        enqueue_decoded_chunk(c, it->second->chunk, pool,
                              it->second->player_modified);
        ++issued;
    }
    return issued;
}

int World::pending_async() const { return jobs_in_flight_.load(); }

void World::enqueue_decoded_chunk(ChunkCoord c, Chunk chunk,
                                  core::ThreadPool& pool,
                                  bool preserve_on_evict) {
    const std::uint64_t stamp = ++request_seq_;
    requested_[c] = stamp;
    jobs_in_flight_.fetch_add(1);
    const std::uint64_t gen = generation_;
    std::uint8_t mask = 0;
    NeighborPlanes planes = neighbor_planes_for(c, &mask);
    const MesherKind kind = mesher_kind_;
    NeighborLight nlight = neighbor_light_for(c);
    pool.submit([this, c, gen, stamp, preserve_on_evict, mask, kind,
                 planes = std::move(planes),
                 nlight = std::move(nlight),
                 chunk = std::move(chunk)]() mutable {
        ZoneScopedN("chunk_loaded_worker_job");
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        FinishedChunk fc;
        fc.coord = c;
        fc.generation = gen;
        fc.request_stamp = stamp;
        fc.chunk = std::move(chunk);
        fc.preserve_on_evict = preserve_on_evict;
        // terrain step is skipped on the load path; the chunk came off disk
        // already populated, so worker time is just the mesh build.
        fc.terrain_ms = 0.0;
        propagate_light(fc.chunk, nlight, fc.light);
        fc.mesh_data  = build_chunk_mesh(kind, fc.chunk, planes,
                                         {&fc.light, &nlight});
        fc.neighbor_mask = mask;
        fc.visibility = compute_section_visibility(fc.chunk);
        fc.worker_ms  = std::chrono::duration<double, std::milli>(
            clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(finished_mutex_);
        finished_.push(std::move(fc));
    });
}

void World::clear_all() {
    chunks_.clear();
    requested_.clear();
    // A full reload replaces world state wholesale; stale stashed edits
    // from the previous state must not leak into it.
    edited_stash_.clear();
    ++generation_;  // in-flight jobs are now stale; drain_finished drops them
    std::lock_guard<std::mutex> lock(finished_mutex_);
    // Results already queued but not yet drained are dropped here, so their
    // in-flight count would leak; account for them. Jobs still running will
    // decrement themselves when they drain (and then be discarded by gen).
    jobs_in_flight_.fetch_sub(static_cast<int>(finished_.size()));
    std::queue<FinishedChunk> empty;
    finished_.swap(empty);
}

void World::for_each_chunk(
    const std::function<void(ChunkCoord, const Chunk&)>& fn) const {
    for (const auto& kv : chunks_) fn(kv.first, kv.second->chunk);
}

BlockId World::block_at(int wx, int wy, int wz) const {
    if (wy < 0 || wy >= kChunkSizeY) return BlockId::Air;
    ChunkCoord cc{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    auto it = chunks_.find(cc);
    if (it == chunks_.end()) return BlockId::Air;
    return it->second->chunk.get(floor_mod(wx, kChunkSizeX), wy,
                                 floor_mod(wz, kChunkSizeZ));
}

bool World::set_block(int wx, int wy, int wz, BlockId b) {
    if (wy < 0 || wy >= kChunkSizeY) return false;
    ChunkCoord cc{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    auto it = chunks_.find(cc);
    if (it == chunks_.end()) return false;

    ChunkSlot& slot = *it->second;
    int lx = floor_mod(wx, kChunkSizeX);
    int lz = floor_mod(wz, kChunkSizeZ);
    if (slot.chunk.get(lx, wy, lz) == b) return false;

    slot.chunk.set(lx, wy, lz, b);
    slot.player_modified = true;
    const auto edit_t0 = std::chrono::steady_clock::now();
    std::uint8_t mask = 0;
    const NeighborPlanes planes = neighbor_planes_for(cc, &mask);
    const NeighborLight nlight = neighbor_light_for(cc);
    // Relight before remeshing: placing or breaking a block changes what
    // the light reaches, and the mesh bakes the result per vertex. This is
    // the whole edit cost, and propagation is 0.04 ms/chunk, so it does not
    // move the block-edit latency figure meaningfully.
    propagate_light(slot.chunk, nlight, slot.light);
    auto mesh_data = build_chunk_mesh(mesher_kind_, slot.chunk, planes,
                                      {&slot.light, &nlight});
    slot.meshed_with = mask;
    // Edits can shift quads across section boundaries (placing a block on
    // top of a tall column, breaking the lowest solid in a section), so
    // every section is re-bucketed and the chunk_aabb gets rebuilt. Greedy
    // meshing on a 16x256x16 chunk is sub-millisecond, so doing it again
    // per edit is fine.
    auto built = bucket_quads_by_section(mesh_data, slot.coord);
    apply_sections(slot, std::move(built), quad_ibo_);
    slot.section_visibility = compute_section_visibility(slot.chunk);
    edit_last_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - edit_t0).count();
    edit_total_ms_ += edit_last_ms_;
    edit_max_ms_ = std::max(edit_max_ms_, edit_last_ms_);
    ++edit_count_;

    // An edit in the outermost column changes what the chunk next door
    // should be hiding: digging into a shared wall exposes a face on the
    // other side of it. Only boundary edits can do that, so only they pay.
    ChunkCoord touched{0, 0};
    bool touches_boundary = false;
    if (lx == 0)                  { touched = {cc.x - 1, cc.z}; touches_boundary = true; }
    else if (lx == kChunkSizeX-1) { touched = {cc.x + 1, cc.z}; touches_boundary = true; }
    if (touches_boundary) queue_remesh(touched);
    if (lz == 0)                  { queue_remesh({cc.x, cc.z - 1}); }
    else if (lz == kChunkSizeZ-1) { queue_remesh({cc.x, cc.z + 1}); }
    return true;
}

// Amanatides-Woo DDA. Steps one voxel along whichever axis next crosses
// an integer plane.
World::RayHit World::raycast(const glm::vec3& origin, const glm::vec3& dir,
                             float max_distance) const {
    RayHit hit;
    glm::vec3 d = dir;
    float len2 = glm::dot(d, d);
    if (len2 < 1e-12f) return hit;
    d /= std::sqrt(len2);

    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    int step_x = (d.x > 0) ? 1 : (d.x < 0 ? -1 : 0);
    int step_y = (d.y > 0) ? 1 : (d.y < 0 ? -1 : 0);
    int step_z = (d.z > 0) ? 1 : (d.z < 0 ? -1 : 0);

    auto next_boundary = [](float p, int step) {
        if (step > 0) return std::floor(p) + 1.0f;
        if (step < 0) return std::floor(p);
        return p;
    };

    constexpr float kInf = std::numeric_limits<float>::infinity();
    float t_max_x = step_x ? (next_boundary(origin.x, step_x) - origin.x) / d.x : kInf;
    float t_max_y = step_y ? (next_boundary(origin.y, step_y) - origin.y) / d.y : kInf;
    float t_max_z = step_z ? (next_boundary(origin.z, step_z) - origin.z) / d.z : kInf;
    float t_delta_x = step_x ? std::abs(1.0f / d.x) : kInf;
    float t_delta_y = step_y ? std::abs(1.0f / d.y) : kInf;
    float t_delta_z = step_z ? std::abs(1.0f / d.z) : kInf;

    float t = 0.0f;
    int last_axis = -1;

    while (t <= max_distance) {
        if (is_solid(block_at(x, y, z))) {
            hit.hit = true;
            hit.block_x = x; hit.block_y = y; hit.block_z = z;
            hit.distance = t;
            if      (last_axis == 0) hit.nx = -step_x;
            else if (last_axis == 1) hit.ny = -step_y;
            else if (last_axis == 2) hit.nz = -step_z;
            return hit;
        }
        if (t_max_x < t_max_y && t_max_x < t_max_z) {
            t = t_max_x; t_max_x += t_delta_x; x += step_x; last_axis = 0;
        } else if (t_max_y < t_max_z) {
            t = t_max_y; t_max_y += t_delta_y; y += step_y; last_axis = 1;
        } else {
            t = t_max_z; t_max_z += t_delta_z; z += step_z; last_axis = 2;
        }
    }
    return hit;
}

void World::debug_dump_visibility(const gfx::Frustum& frustum) const {
    int drawn = 0;
    for (const auto& kv : chunks_) {
        const ChunkSlot& s = *kv.second;
        bool vis = frustum.intersects_aabb(s.chunk_aabb);
        if (vis) ++drawn;
        int meshed = 0;
        for (const auto& sec : s.sections) meshed += sec.has_mesh ? 1 : 0;
        std::printf("  chunk (%+3d,%+3d)  %s  aabb y[%.0f..%.0f] xz[%.0f,%.0f..%.0f,%.0f] sections=%d solids=%d\n",
                    s.coord.x, s.coord.z, vis ? "VISIBLE" : "culled",
                    s.chunk_aabb.min.y, s.chunk_aabb.max.y,
                    s.chunk_aabb.min.x, s.chunk_aabb.min.z,
                    s.chunk_aabb.max.x, s.chunk_aabb.max.z,
                    meshed, s.chunk.solid_count());
    }
    std::printf("  total visible: %d / %zu\n", drawn, chunks_.size());
}

namespace {

// True if section `i` of a chunk should draw given the chunk's reachable
// mask. Greedy quads bucket by their bottom Y, so a section's AABB can
// extend above its own slab - the section must draw if ANY slab its AABB
// spans is reachable, or a tall cliff face would vanish when only its
// upper half is in view.
bool section_reachable(std::uint8_t mask, int i, const gfx::AABB& aabb) {
    // Every section from this one upward, NOT just the ones this section's
    // geometry happens to reach.
    //
    // The bound used to come from aabb.max.y, which made a visibility test
    // depend on how much geometry a section held: a section with a tall
    // quad in it searched further up the mask than one without, and was
    // therefore more likely to survive. That was never sound, and it was
    // being propped up by chunk-boundary walls - the tallest quads in most
    // sections. Cross-chunk culling deleted those walls, the search ranges
    // collapsed, and sections holding visible geometry started being
    // culled: the byte-identity check caught it as 267 sections drawn
    // against 407, with the images no longer matching.
    //
    // A section's geometry is bucketed by its BOTTOM y and can extend
    // arbitrarily far up, so any reachable section at or above it can be
    // showing part of it. The aabb parameter is kept for the signature the
    // tests use and deliberately no longer consulted.
    (void)aabb;
    for (int s = i; s < kSectionsPerChunk; ++s) {
        if (mask & (1u << s)) return true;
    }
    return false;
}

bool section_box_in_frustum(const gfx::Frustum& frustum, ChunkCoord c, int sy) {
    const float ox = static_cast<float>(c.x * kChunkSizeX);
    const float oz = static_cast<float>(c.z * kChunkSizeZ);
    const float oy = static_cast<float>(sy * kSectionHeight);
    return frustum.intersects_aabb({{ox, oy, oz},
                                    {ox + kChunkSizeX, oy + kSectionHeight,
                                     oz + kChunkSizeZ}});
}

}  // namespace

bool section_reachable_in_mask(std::uint8_t mask, int sy, const gfx::AABB& aabb) {
    return section_reachable(mask, sy, aabb);
}

bool occlusion_bfs(
    const glm::vec3& camera_pos,
    const gfx::Frustum& frustum,
    const std::function<const SectionVisArray*(ChunkCoord)>& visibility_of,
    SectionReachableMap& reachable) {
    const int wx = static_cast<int>(std::floor(camera_pos.x));
    const int wz = static_cast<int>(std::floor(camera_pos.z));
    const ChunkCoord start{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    if (!visibility_of(start)) return false;

    // Clamping Y keeps a camera above the build limit (or below bedrock)
    // working: it seeds from the nearest section with unconstrained exits,
    // which is conservative.
    const int start_sy = std::clamp(
        static_cast<int>(std::floor(camera_pos.y)) / kSectionHeight,
        0, kSectionsPerChunk - 1);

    struct Node {
        ChunkCoord   c;
        std::int8_t  sy;
        std::int8_t  entry_face;  // face of THIS section we entered through; -1 at seed
        std::uint8_t dirs;        // directions taken on the path so far
    };
    std::vector<Node> queue;
    queue.reserve(512);

    // Frustum-test is skipped for the seed: the camera sits inside it.
    reachable[start] |= static_cast<std::uint8_t>(1u << start_sy);
    queue.push_back({start, static_cast<std::int8_t>(start_sy), -1, 0});

    std::size_t head = 0;
    while (head < queue.size()) {
        const Node n = queue[head++];
        const SectionVisArray* vis = visibility_of(n.c);  // non-null: checked at enqueue

        for (int d = 0; d < 6; ++d) {
            // Never step back along an axis direction the path already used
            // in reverse - stops sightlines that would have to bend around
            // a corner and come back.
            if (n.dirs & (1u << opposite_face(d))) continue;
            if (n.entry_face >= 0 &&
                !faces_connected((*vis)[n.sy], n.entry_face, d)) continue;

            ChunkCoord nc = n.c;
            int nsy = n.sy;
            switch (d) {
                case kFaceNegX: nc.x -= 1; break;
                case kFacePosX: nc.x += 1; break;
                case kFaceNegY: nsy -= 1;  break;
                case kFacePosY: nsy += 1;  break;
                case kFaceNegZ: nc.z -= 1; break;
                case kFacePosZ: nc.z += 1; break;
            }
            if (nsy < 0 || nsy >= kSectionsPerChunk) continue;
            if (!visibility_of(nc)) continue;

            auto& mask = reachable[nc];
            const auto bit = static_cast<std::uint8_t>(1u << nsy);
            if (mask & bit) continue;
            if (!section_box_in_frustum(frustum, nc, nsy)) continue;
            mask |= bit;
            queue.push_back({nc, static_cast<std::int8_t>(nsy),
                             static_cast<std::int8_t>(opposite_face(d)),
                             static_cast<std::uint8_t>(n.dirs | (1u << d))});
        }
    }
    return true;
}

int World::debug_validate_gpu_meshes() const {
    std::vector<gfx::VertexPacked> verts;
    std::vector<std::uint32_t> idx;
    int bad = 0;
    for (const auto& kv : chunks_) {
        const ChunkSlot& slot = *kv.second;
        if (!slot.any_section_has_mesh) continue;
        slot.chunk_mesh.debug_read_back(verts, idx);
        for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
            const glm::vec3 p0 = verts[idx[t]].pos();
            const glm::vec3 p1 = verts[idx[t + 1]].pos();
            const glm::vec3 p2 = verts[idx[t + 2]].pos();
            const glm::vec3 n = verts[idx[t]].nrm();
            const int d = (std::abs(n.x) > 0.5f) ? 0 : (std::abs(n.y) > 0.5f ? 1 : 2);
            const bool coplanar = (p0[d] == p1[d]) && (p1[d] == p2[d]);
            bool backed = false;
            if (coplanar) {
                const int s = static_cast<int>(std::lround(p0[d]));
                const int u_axis = (d + 1) % 3, v_axis = (d + 2) % 3;
                const int u0 = static_cast<int>(std::floor(std::min({p0[u_axis], p1[u_axis], p2[u_axis]})));
                const int u1 = static_cast<int>(std::ceil (std::max({p0[u_axis], p1[u_axis], p2[u_axis]})));
                const int v0 = static_cast<int>(std::floor(std::min({p0[v_axis], p1[v_axis], p2[v_axis]})));
                const int v1 = static_cast<int>(std::ceil (std::max({p0[v_axis], p1[v_axis], p2[v_axis]})));
                for (int u = u0; u < u1 && !backed; ++u) {
                    for (int v = v0; v < v1 && !backed; ++v) {
                        int cell[3];
                        cell[u_axis] = u; cell[v_axis] = v;
                        cell[d] = (n[d] > 0.0f) ? s - 1 : s;
                        if (in_chunk_bounds(cell[0], cell[1], cell[2]) &&
                            is_solid(slot.chunk.get(cell[0], cell[1], cell[2]))) backed = true;
                    }
                }
            }
            if (!coplanar || !backed) {
                ++bad;
                if (bad <= 16) {
                    std::printf("[validate] chunk(%+d,%+d) tri %zu %s: "
                                "(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f) n=(%.0f,%.0f,%.0f) ids=%u,%u,%u\n",
                                slot.coord.x, slot.coord.z, t / 3,
                                coplanar ? "unbacked" : "NON-COPLANAR",
                                p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z,
                                n.x, n.y, n.z, idx[t], idx[t+1], idx[t+2]);
                }
            }
        }
    }
    std::printf("[validate] GPU mesh triangles flagged: %d\n", bad);
    return bad;
}

DrawStats World::draw_impl(const gfx::Frustum& frustum,
                           const SectionReachableMap* reachable,
                           const std::function<void(const glm::mat4&)>& set_model) const {
    DrawStats stats;
    stats.chunks_total   = static_cast<int>(chunks_.size());
    // Deterministic draw order. unordered_map iteration order is
    // unspecified: it follows bucket layout and shifts on rehash, so it
    // varies with insertion history and therefore with which worker
    // finished each chunk first. Two identical runs then draw in
    // different orders and MSAA resolves seam pixels differently --
    // which breaks the byte-stable screenshot guarantee the occlusion A/B
    // verification depends on. Sorting by coord is O(n log n) on the
    // resident set (~625 chunks) once per pass, well under the noise floor.
    draw_order_.clear();
    draw_order_.reserve(chunks_.size());
    for (const auto& kv : chunks_) draw_order_.push_back(kv.second.get());
    std::sort(draw_order_.begin(), draw_order_.end(),
              [](const ChunkSlot* a, const ChunkSlot* b) {
                  return a->coord.x != b->coord.x ? a->coord.x < b->coord.x
                                                  : a->coord.z < b->coord.z;
              });
    for (const ChunkSlot* slot_ptr : draw_order_) {
        const ChunkSlot& slot = *slot_ptr;
        if (!slot.any_section_has_mesh) continue;
        if (!frustum.intersects_aabb(slot.chunk_aabb)) continue;

        std::uint8_t reach_mask = 0xFF;
        if (reachable) {
            auto it = reachable->find(slot.coord);
            reach_mask = (it != reachable->end()) ? it->second : 0;
        }

        const float ox = static_cast<float>(slot.coord.x * kChunkSizeX);
        const float oz = static_cast<float>(slot.coord.z * kChunkSizeZ);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), {ox, 0.0f, oz});
        bool vao_bound = false;
        bool drew_any  = false;
        for (int i = 0; i < kSectionsPerChunk; ++i) {
            const auto& sec = slot.sections[i];
            if (!sec.has_mesh) continue;
            if (!frustum.intersects_aabb(sec.aabb)) continue;
            if (reachable && !section_reachable(reach_mask, i, sec.aabb)) {
                ++stats.sections_occluded;
                continue;
            }
            if (!vao_bound) {
                set_model(model);
                slot.chunk_mesh.bind();
                vao_bound = true;
            }
            slot.chunk_mesh.draw_range_bound(sec.index_offset, sec.index_count);
            ++stats.sections_drawn;
            stats.triangles_drawn += sec.index_count / 3;
            drew_any = true;
        }
        if (drew_any) ++stats.chunks_drawn;
    }
    return stats;
}

DrawStats World::draw_visible(const gfx::Frustum& frustum,
                              const gfx::Shader& shader) const {
    return draw_impl(frustum, nullptr,
        [&](const glm::mat4& m) { shader.set_mat4("u_model", m); });
}

DrawStats World::draw_visible_with(const gfx::Frustum& frustum,
    std::function<void(const glm::mat4& model)> set_model) const {
    return draw_impl(frustum, nullptr, set_model);
}

DrawStats World::draw_visible_occluded(const gfx::Frustum& frustum,
                                       const glm::vec3& camera_pos,
                                       const gfx::Shader& shader) const {
    ZoneScopedN("occlusion_bfs");
    SectionReachableMap reachable;
    const bool ok = occlusion_bfs(
        camera_pos, frustum,
        [this](ChunkCoord c) -> const SectionVisArray* {
            auto it = chunks_.find(c);
            return it == chunks_.end() ? nullptr
                                       : &it->second->section_visibility;
        },
        reachable);
    if (!ok) return draw_visible(frustum, shader);
    return draw_impl(frustum, &reachable,
        [&](const glm::mat4& m) { shader.set_mat4("u_model", m); });
}

}  // namespace world
