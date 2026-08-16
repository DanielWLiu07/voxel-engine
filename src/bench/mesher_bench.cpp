#include "bench/mesher_bench.h"

#include <glm/glm.hpp>

#include "gfx/camera.h"
#include "gfx/frustum.h"
#include "gfx/mesh.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/section_visibility.h"
#include "world/terrain_gen.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace bench {

namespace {

// Greedy meshing against the naive one-quad-per-face baseline, on a
// single chunk. Self-contained: it generates its own terrain and
// shares no state with the cull or footprint benchmarks below.
//
// Returns the contiguous-terrain quad ratio, which is the figure the
// CI gate reads. It used to be scraped back out of the printed prose.
double bench_greedy_vs_naive() {
    double ci_gate_ratio = 0.0;
constexpr int kRuns = 25;

auto bench_one = [&](bool caves, const char* label) {
    world::TerrainGen terrain(1337);
    terrain.set_caves_enabled(caves);
    world::Chunk chunk;
    terrain.fill_chunk(0, 0, chunk);

    double naive_total = 0.0, greedy_total = 0.0;
    world::ChunkMeshData last_naive, last_greedy;
    for (int i = 0; i < kRuns; ++i) {
        last_naive  = world::build_chunk_mesh_naive(chunk);
        last_greedy = world::build_chunk_mesh_greedy(chunk);
        naive_total  += last_naive.build_ms;
        greedy_total += last_greedy.build_ms;
    }
    std::size_t naive_tris  = static_cast<std::size_t>(last_naive.quad_count) * 2;
    std::size_t greedy_tris = static_cast<std::size_t>(last_greedy.quad_count) * 2;

    std::printf("---- %s ----\n", label);
    std::printf("naive : quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                last_naive.quad_count, naive_tris, naive_total / kRuns);
    std::printf("greedy: quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                last_greedy.quad_count, greedy_tris, greedy_total / kRuns);
    // GPU buffer footprint: the merged mesh uploads fewer vertices, so
    // the triangle win is a memory win too. Vertex bytes only - the
    // quad index pattern is one shared buffer engine-wide, not a
    // per-chunk cost.
    auto vram_kb = [](const world::ChunkMeshData& m) {
        return m.vertices.size() * sizeof(gfx::VertexPacked) / 1024.0;
    };
    const double naive_kb = vram_kb(last_naive);
    const double greedy_kb = vram_kb(last_greedy);
    std::printf("vram  : naive=%6.1f KB  greedy=%6.1f KB\n",
                naive_kb, greedy_kb);
    if (last_greedy.quad_count > 0 && greedy_tris > 0) {
        const double quad_ratio =
            static_cast<double>(last_naive.quad_count) / last_greedy.quad_count;
        if (!caves) ci_gate_ratio = quad_ratio;
        std::printf("ratio : %.1fx fewer quads  |  %.1fx fewer tris"
                    "  |  %.1fx less vram\n",
                    quad_ratio,
                    static_cast<double>(naive_tris)             / greedy_tris,
                    greedy_kb > 0.0 ? naive_kb / greedy_kb : 0.0);
    }
};

std::printf("==== chunk mesher benchmark (%d runs, Perlin terrain chunk 0,0) ====\n", kRuns);
// Caves-off measures the greedy algorithm against contiguous terrain
// - this is what the CI gate checks. Caves-on is the realistic
// gameplay path; lower ratio is expected because caves break up
// mergeable face runs.
bench_one(/*caves=*/false, "contiguous terrain (CI gate)");
bench_one(/*caves=*/true,  "with caves (gameplay terrain)");
    return ci_gate_ratio;
}

}  // namespace

int run_mesher_bench(int stream_radius) {
    const double greedy_ratio = bench_greedy_vs_naive();


    // ---- Frustum cull benchmark ---------------------------------------
    // CPU-only, deterministic. Generates a 25x25 chunk grid and counts how
    // many AABBs the view frustum keeps, under four (AABB, far-plane)
    // combinations so the improvement from tightening either dimension is
    // visible side-by-side. No GL context needed.
    const int kRadius = stream_radius;
    const int side = 2 * kRadius + 1;
    const int total = side * side;
    const float kFogEnd   = static_cast<float>(kRadius * world::kChunkSizeX) * 0.95f;
    const float kFarTight = kFogEnd + static_cast<float>(world::kChunkSizeX);
    constexpr float kFovDeg = 70.0f;
    constexpr float kAspect = 16.0f / 9.0f;

    world::TerrainGen cull_terrain(1337);
    std::vector<gfx::AABB> wide_aabbs;
    std::vector<gfx::AABB> tight_aabbs;
    std::vector<std::array<world::SectionBounds, world::kSectionsPerChunk>> section_bounds;
    std::vector<world::SectionVisArray> vis_arrays;
    wide_aabbs.reserve(total);
    tight_aabbs.reserve(total);
    section_bounds.reserve(total);
    vis_arrays.reserve(total);
    int total_sections_nonempty = 0;
    // Whole-world GPU mesh footprint, accumulated over the same chunks the
    // cull bench generates. World::apply_sections buckets one chunk-wide
    // greedy mesh into sections without re-meshing, so the vertices summed
    // here are exactly the vertices the engine uploads - no GL context
    // needed to get the number that matters.
    std::size_t world_greedy_quads = 0;
    std::size_t world_naive_quads  = 0;
    std::size_t world_max_quads    = 0;  // sizes the one shared index buffer
    // Cave pose for the occlusion bench: first 2-tall air pocket with a
    // solid roof and floor, scanned in fixed order near the origin so the
    // pose is deterministic for a given seed.
    bool cave_found = false;
    glm::vec3 cave_pos{};
    auto gen_t0 = std::chrono::steady_clock::now();
    for (int cz = -kRadius; cz <= kRadius; ++cz) {
        for (int cx = -kRadius; cx <= kRadius; ++cx) {
            world::Chunk c;
            cull_terrain.fill_chunk(cx, cz, c);
            const float ox = static_cast<float>(cx * world::kChunkSizeX);
            const float oz = static_cast<float>(cz * world::kChunkSizeZ);
            wide_aabbs.push_back({{ox, 0.0f, oz},
                                  {ox + world::kChunkSizeX,
                                   static_cast<float>(world::kChunkSizeY),
                                   oz + world::kChunkSizeZ}});
            tight_aabbs.push_back(world::make_chunk_aabb({cx, cz}, c));

            const auto greedy = world::build_chunk_mesh_greedy(c);
            const auto naive  = world::build_chunk_mesh_naive(c);
            const std::size_t gq = greedy.vertices.size() / 4;
            world_greedy_quads += gq;
            world_naive_quads  += naive.vertices.size() / 4;
            world_max_quads = std::max(world_max_quads, gq);

            auto secs = world::compute_section_bounds({cx, cz}, c);
            for (const auto& s : secs) if (s.has_mesh) ++total_sections_nonempty;
            section_bounds.push_back(std::move(secs));
            vis_arrays.push_back(world::compute_section_visibility(c));

            if (!cave_found && std::abs(cx) <= 2 && std::abs(cz) <= 2) {
                for (int y = 10; y <= 40 && !cave_found; ++y) {
                    for (int z = 0; z < world::kChunkSizeZ && !cave_found; ++z) {
                        for (int x = 0; x < world::kChunkSizeX; ++x) {
                            if (world::is_solid(c.get(x, y, z)))     continue;
                            if (world::is_solid(c.get(x, y + 1, z))) continue;
                            if (!world::is_solid(c.get_or_air(x, y + 2, z))) continue;
                            if (!world::is_solid(c.get_or_air(x, y - 1, z))) continue;
                            cave_pos = {ox + x + 0.5f,
                                        static_cast<float>(y) + 1.5f,
                                        oz + z + 0.5f};
                            cave_found = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    auto gen_t1 = std::chrono::steady_clock::now();
    const double gen_ms = std::chrono::duration<double, std::milli>(gen_t1 - gen_t0).count();

    // Pose roughly matches the README's "gameplay viewpoint": mid-air over
    // the origin, looking down -Z with a slight downward pitch.
    gfx::FlyCamera cam;
    cam.set_position({0.0f, 80.0f, 0.0f});
    cam.set_yaw_pitch(-90.0f, -15.0f);
    const glm::mat4 view = cam.view_matrix();

    auto count_visible = [&](const std::vector<gfx::AABB>& boxes, float zfar) {
        gfx::Frustum f;
        f.from_view_proj(cam.proj_matrix(kAspect, kFovDeg, 0.1f, zfar) * view);
        int drawn = 0;
        for (const auto& b : boxes) if (f.intersects_aabb(b)) ++drawn;
        return drawn;
    };

    const int wide_far500   = count_visible(wide_aabbs,  500.0f);
    const int tight_far500  = count_visible(tight_aabbs, 500.0f);
    const int wide_fartight = count_visible(wide_aabbs,  kFarTight);
    const int tight_fartight= count_visible(tight_aabbs, kFarTight);

    // Section-level cull: same frustum, but each visible chunk's sections
    // are tested individually. Mirrors what World::draw_visible_with does.
    // The one section-counting body; the occlusion comparison below feeds
    // it the surface pose's frustum, so both lines measure with literally
    // the same code.
    auto count_frustum_sections = [&](const gfx::Frustum& f) {
        int drawn = 0;
        for (std::size_t i = 0; i < tight_aabbs.size(); ++i) {
            if (!f.intersects_aabb(tight_aabbs[i])) continue;
            for (const auto& s : section_bounds[i]) {
                if (s.has_mesh && f.intersects_aabb(s.aabb)) ++drawn;
            }
        }
        return drawn;
    };
    gfx::Frustum tight_section_f;
    tight_section_f.from_view_proj(
        cam.proj_matrix(kAspect, kFovDeg, 0.1f, kFarTight) * view);
    const int sections_drawn = count_frustum_sections(tight_section_f);

    // ---- Occlusion cull (section-graph BFS) ---------------------------
    // Same drawn-section count as above, but only sections the camera can
    // reach through air survive. Surface pose reuses the frustum camera;
    // cave pose drops the camera into the air pocket found during gen.
    auto make_frustum = [&](const glm::vec3& pos, float yaw, float pitch) {
        gfx::FlyCamera c2;
        c2.set_position(pos);
        c2.set_yaw_pitch(yaw, pitch);
        gfx::Frustum f;
        f.from_view_proj(c2.proj_matrix(kAspect, kFovDeg, 0.1f, kFarTight)
                         * c2.view_matrix());
        return f;
    };
    auto chunk_index_of = [&](world::ChunkCoord c) -> int {
        if (c.x < -kRadius || c.x > kRadius ||
            c.z < -kRadius || c.z > kRadius) return -1;
        return (c.z + kRadius) * side + (c.x + kRadius);
    };
    // Returns -1 if the BFS refused to run (camera chunk unloaded - can't
    // happen for these poses, but keep the contract visible).
    auto count_occl_sections = [&](const glm::vec3& cam_pos, const gfx::Frustum& f) {
        world::SectionReachableMap reachable;
        const bool ok = world::occlusion_bfs(
            cam_pos, f,
            [&](world::ChunkCoord c) -> const world::SectionVisArray* {
                const int idx = chunk_index_of(c);
                return idx < 0 ? nullptr : &vis_arrays[idx];
            },
            reachable);
        if (!ok) return -1;
        int drawn = 0;
        for (int cz = -kRadius; cz <= kRadius; ++cz) {
            for (int cx = -kRadius; cx <= kRadius; ++cx) {
                const int idx = (cz + kRadius) * side + (cx + kRadius);
                if (!f.intersects_aabb(tight_aabbs[idx])) continue;
                auto it = reachable.find({cx, cz});
                const std::uint8_t mask =
                    (it == reachable.end()) ? std::uint8_t(0) : it->second;
                for (int sy = 0; sy < world::kSectionsPerChunk; ++sy) {
                    const auto& s = section_bounds[idx][sy];
                    if (!s.has_mesh || !f.intersects_aabb(s.aabb)) continue;
                    if (world::section_reachable_in_mask(mask, sy, s.aabb)) ++drawn;
                }
            }
        }
        return drawn;
    };

    const glm::vec3  surface_pos{0.0f, 80.0f, 0.0f};
    const gfx::Frustum surface_f = make_frustum(surface_pos, -90.0f, -15.0f);
    const int surf_frustum = count_frustum_sections(surface_f);
    const int surf_occl    = count_occl_sections(surface_pos, surface_f);

    int cave_frustum = -1, cave_occl = -1;
    if (cave_found) {
        const gfx::Frustum cave_f = make_frustum(cave_pos, -90.0f, 0.0f);
        cave_frustum = count_frustum_sections(cave_f);
        cave_occl    = count_occl_sections(cave_pos, cave_f);
    }

    auto ratio = [&](int drawn, int denom) { return drawn > 0 ? double(denom)/drawn : 0.0; };

    std::printf("\n==== frustum cull benchmark (radius %d, %d chunks, pos (0,80,0), yaw -90, pitch -15, fov %.0f) ====\n",
                kRadius, total, kFovDeg);
    std::printf("(grid built in %.1f ms)\n", gen_ms);
    std::printf("chunk-level cull:\n");
    std::printf("  wide AABB,  far 500 m  : %3d/%d drawn  (%.2fx)   <- baseline (matches old README)\n",
                wide_far500, total, ratio(wide_far500, total));
    std::printf("  tight AABB, far 500 m  : %3d/%d drawn  (%.2fx)\n",
                tight_far500, total, ratio(tight_far500, total));
    std::printf("  wide AABB,  far %3.0f m  : %3d/%d drawn  (%.2fx)\n",
                kFarTight, wide_fartight, total, ratio(wide_fartight, total));
    std::printf("  tight AABB, far %3.0f m  : %3d/%d drawn  (%.2fx)   <- chunk-level final\n",
                kFarTight, tight_fartight, total, ratio(tight_fartight, total));
    std::printf("section-level cull (32-block vertical sections, tight AABB, far %3.0f m):\n", kFarTight);
    std::printf("  vs non-empty sections    : %4d / %d  (%.2fx)   <- per-section cull ratio\n",
                sections_drawn, total_sections_nonempty,
                ratio(sections_drawn, total_sections_nonempty));
    std::printf("  vs all loaded sections   : %4d / %d  (%.2fx)   <- vs naive 'draw every section'\n",
                sections_drawn, total * world::kSectionsPerChunk,
                ratio(sections_drawn, total * world::kSectionsPerChunk));
    std::printf("occlusion cull (section-graph BFS, frustum+occlusion vs frustum-only):\n");
    std::printf("  surface pose (0,80,0)    : %4d -> %4d sections  (%.2fx fewer)\n",
                surf_frustum, surf_occl,
                surf_occl > 0 ? double(surf_frustum) / surf_occl : 0.0);
    if (cave_found && cave_occl >= 0) {
        std::printf("  cave pose (%.1f,%.1f,%.1f)     : %4d -> %4d sections  (%.2fx fewer)\n",
                    cave_pos.x, cave_pos.y, cave_pos.z,
                    cave_frustum, cave_occl,
                    cave_occl > 0 ? double(cave_frustum) / cave_occl : 0.0);
    } else {
        std::printf("  cave pose                : n/a (no air pocket found near origin)\n");
    }

    // ---- Whole-world GPU mesh footprint --------------------------------
    // The README's headline memory figure used to be readable only off the
    // debug HUD of a running window, which made it the one number in the
    // memory story that CI could not check and a reader could not
    // reproduce. It is pure arithmetic over the meshes, so compute it here
    // with the rest of the headless bench.
    //
    // Each row adds one optimization to the row above it, so the cost of
    // dropping any single one is the difference between two adjacent rows.
    constexpr std::size_t kLegacyVertexBytes = 40;  // the pre-packing float layout
    constexpr std::size_t kIndexBytesPerQuad = 6 * sizeof(std::uint32_t);
    const std::size_t packed_vertex_bytes = sizeof(gfx::VertexPacked);

    // The shared buffer is sized by QuadIndexBuffer::bind_for, which grows
    // to 1.5x the request so the next slightly-bigger chunk doesn't rebuild
    // the pattern. Applying that policy to the largest chunk mesh gives the
    // capacity the engine settles at, for any upload order in which the
    // largest chunk is not the very first one to arrive.
    const std::size_t shared_index_quads =
        std::max(world_max_quads + world_max_quads / 2, std::size_t{1024});

    auto mb = [](std::size_t bytes) { return bytes / (1024.0 * 1024.0); };
    const double naive_40_mb = mb(world_naive_quads * 4 * kLegacyVertexBytes +
                                  world_naive_quads * kIndexBytesPerQuad);
    const double greedy_40_mb = mb(world_greedy_quads * 4 * kLegacyVertexBytes +
                                   world_greedy_quads * kIndexBytesPerQuad);
    const double greedy_packed_mb = mb(world_greedy_quads * 4 * packed_vertex_bytes +
                                       world_greedy_quads * kIndexBytesPerQuad);
    const double shipped_mb = mb(world_greedy_quads * 4 * packed_vertex_bytes +
                                 shared_index_quads * kIndexBytesPerQuad);

    std::printf("\n==== whole-world GPU mesh footprint (radius %d, %d chunks) ====\n",
                kRadius, total);
    std::printf("(vertex bytes + index bytes; row 4 is what the engine holds "
                "resident, computed with no GL context)\n");
    std::printf("  1. naive faces, %zu B vertex, per-chunk index buffer : %9zu quads  %8.1f MB\n",
                kLegacyVertexBytes, world_naive_quads, naive_40_mb);
    std::printf("  2. + greedy meshing                                  : %9zu quads  %8.1f MB\n",
                world_greedy_quads, greedy_40_mb);
    std::printf("  3. + %zu B packed vertex                              : %9zu quads  %8.1f MB\n",
                packed_vertex_bytes, world_greedy_quads, greedy_packed_mb);
    std::printf("  4. + one shared quad index buffer  <- shipped        : %9zu quads  %8.1f MB\n",
                world_greedy_quads, shipped_mb);
    std::printf("  greedy %.1fx  |  packing %.2fx  |  shared index %.2fx  |  all three %.1fx\n",
                greedy_40_mb > 0.0 ? naive_40_mb / greedy_40_mb : 0.0,
                greedy_packed_mb > 0.0 ? greedy_40_mb / greedy_packed_mb : 0.0,
                shipped_mb > 0.0 ? greedy_packed_mb / shipped_mb : 0.0,
                shipped_mb > 0.0 ? naive_40_mb / shipped_mb : 0.0);

    // Stable, machine-readable summary line so CI can gate the cull ratios
    // without fishing through the prose. Whitespace-separated key=value
    // pairs after a fixed prefix.
    std::printf("\nBENCH_SUMMARY"
                " greedy=%.2f"
                " chunk_tight=%.2f"
                " section_nonempty=%.2f"
                " section_total=%.2f"
                " occl_surface=%.2f"
                " occl_cave=%.2f"
                " vertex_bytes=%zu"
                " world_mesh_mb=%.2f"
                " world_mesh_prepack_mb=%.2f"
                " world_mesh_naive_mb=%.2f\n",
                greedy_ratio,
                ratio(tight_fartight, total),
                ratio(sections_drawn, total_sections_nonempty),
                ratio(sections_drawn, total * world::kSectionsPerChunk),
                surf_occl > 0 ? double(surf_frustum) / surf_occl : 0.0,
                (cave_found && cave_occl > 0) ? double(cave_frustum) / cave_occl : 0.0,
                packed_vertex_bytes, shipped_mb, greedy_packed_mb, naive_40_mb);
    return EXIT_SUCCESS;
}

}  // namespace bench
