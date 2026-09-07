#pragma once

#include "core/thread_pool.h"
#include "gfx/frustum.h"
#include "gfx/mesh.h"
#include "gfx/shader.h"
#include "world/chunk.h"
#include "world/chunk_light.h"
#include "world/chunk_mesh.h"
#include "world/section_visibility.h"
#include "world/terrain_gen.h"
#include "world/terrain_gen4d.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace world {

// A chunk, plus which slice of the fourth dimension it belongs to.
//
// Edits belong to the w they were made at. Without that, an edit made at
// one slice reappears at every other one, in a place where the terrain
// around it means something completely different - a hole dug into a
// hillside at w=0 turning up in mid-air at w=5. In the 3D engine w is
// always 0 and this behaves exactly as a bare ChunkCoord did.
// Which slice an edit belongs to: a chunk and an integer w.
//
// Deliberately NOT the orientation. An edit made on a flat slice is
// restored on a tilted one, and that is a decision rather than an
// oversight: the player built it, and turning their view of the fourth
// dimension should not delete their house. Keying on theta as well would
// make anything built vanish the moment the wheel moved, which is the
// opposite failure and a worse one.
//
// The cost is that two hyperplanes sharing an integer w share a bucket,
// so a structure built flat reappears in terrain that a tilted cut has
// rearranged around it. That is the same trade the w key already makes
// at coarser grain, and it is the reason the w key is integer at all:
// edits belong to a slab you can return to, not to an exact real number
// nobody can hit twice.
struct SliceCoord {
    ChunkCoord c{};
    std::int32_t w = 0;
    bool operator==(const SliceCoord& o) const { return c == o.c && w == o.w; }
};

struct SliceCoordHash {
    std::size_t operator()(const SliceCoord& s) const noexcept {
        const std::size_t h = ChunkCoordHash{}(s.c);
        return h ^ (static_cast<std::size_t>(static_cast<std::uint32_t>(s.w))
                    * 0x9E3779B97F4A7C15ull);
    }
};

// Section constants (kSectionHeight, kSectionsPerChunk) live in chunk.h.
// Each section has its own tight AABB and is culled independently - the
// chunk AABB is the union.

// One section's slice of its chunk's shared mesh: an index range into the
// chunk EBO + the section's own world-space AABB for culling. Sharing one
// VBO per chunk (instead of one per section) is what keeps load-time GL
// throughput on par with the pre-section pipeline.
struct ChunkSection {
    gfx::AABB    aabb{};
    std::uint32_t index_offset = 0;
    std::uint32_t index_count  = 0;
    bool          has_mesh     = false;
};

struct ChunkSlot {
    ChunkCoord coord{};
    Chunk      chunk;
    // One mesh per chunk; sections index into it via (index_offset, index_count).
    gfx::Mesh  chunk_mesh;
    std::array<ChunkSection, kSectionsPerChunk> sections{};
    // Per-section face-pair connectivity for occlusion culling. Computed on
    // the worker next to the greedy mesh; consumed by occlusion_bfs.
    SectionVisArray section_visibility{};
    // Union of section AABBs - the chunk-level fast-path test. If this misses
    // the frustum, we skip all section tests for the chunk.
    gfx::AABB  chunk_aabb{};
    bool       any_section_has_mesh = false;
    // The w this chunk's voxels were generated at.
    //
    // Per chunk, not per world, and that is what lets travel along w be
    // continuous. With a single world-wide meshed_w the only way to move
    // was to rebuild every chunk at once and wait: the terrain updated in
    // discrete full-window waves a third of a second apart. Tracking it
    // here lets the engine always be rebuilding whichever chunks have
    // drifted furthest from the player's w, nearest first, on a per-frame
    // budget - so the world updates continuously instead of pulsing.
    float      slice_w = 0.0f;
    // The slice ROTATION this chunk was generated at. Tracked alongside w
    // because rotating the cut changes the world exactly as translating it
    // does, so a chunk is stale if either has moved.
    float      slice_theta = 0.0f;
    // The slice-z origin this chunk was generated with. Part of the
    // slice's identity like w and theta, so drift cannot be computed
    // without it.
    float      slice_z_shift = 0.0f;
    // The second rotation plane, stamped like the first. Without these a
    // chunk compares against a phi of 0 forever, so any XW turn leaves
    // the whole window permanently stale and the world never converges.
    float      slice_phi = 0.0f;
    float      slice_x_shift = 0.0f;
    // True only for chunks that came off disk. The terrain generator
    // cannot reproduce those, so they are the one case that still has to
    // be stashed whole; everything else is regenerated and has its edits
    // replayed on top.
    bool       from_disk = false;
    // Bytes this chunk holds in GPU buffers (VBO + EBO): the actual vertex
    // and index data uploaded for it. Summed across resident chunks to get
    // the engine's GPU mesh footprint, the VRAM analogue of RSS.
    std::size_t gpu_bytes = 0;
    // Which of the four horizontal neighbours were resident when this
    // slot's mesh was built, as kNeighbor* bits. A chunk meshed before a
    // neighbour arrived still carries the boundary faces that neighbour
    // hides, so it has to be re-meshed once the neighbour lands; this is
    // what says whether that is still owed.
    std::uint8_t meshed_with = 0;
    // Block light for this chunk, flood-filled on the worker next to the
    // mesh. Kept on the slot so a neighbour can be lit from it later.
    LightGrid light;
    // Set by set_block (and for chunks that came off disk rather than the
    // terrain generator). Streaming eviction stashes modified chunks so
    // walking away from an edit can never silently regenerate it.
    bool player_modified = false;
};

// Tight per-chunk AABB: XZ from the chunk's world origin, Y from the actual
// min/max of solid blocks (closed range, +1 on max). Used for a chunk with
// no mesh at all, and by the cull benchmark, which needs an AABB without
// going through the GL-backed ChunkSlot path. Note the renderer culls
// against ChunkSlot::chunk_aabb, which apply_sections builds as the union
// of the section AABBs: the two coincide closely on real terrain but they
// are not the same object.
gfx::AABB make_chunk_aabb(ChunkCoord coord, const Chunk& chunk);

// One row of the bench's section-AABB readout: the world-space AABB plus
// whether the section actually contains any meshed quads (empty sections
// shouldn't count against the cull ratio).
struct SectionBounds {
    gfx::AABB aabb{};
    bool      has_mesh = false;
};

// Runs the greedy mesher + the same per-section bucketing the renderer uses
// and returns just the section AABBs. CPU-only, no GL needed - meant for
// --bench, not the hot path.
std::array<SectionBounds, kSectionsPerChunk>
compute_section_bounds(ChunkCoord coord, const Chunk& chunk);

struct DrawStats {
    int chunks_total = 0;
    int chunks_drawn = 0;
    int sections_drawn = 0;
    // Sections that passed the frustum test but were skipped because the
    // occlusion BFS couldn't reach them through air. 0 on frustum-only paths.
    int sections_occluded = 0;
    std::size_t triangles_drawn = 0;
};

// Sections reachable from the camera, one bitmask per chunk (bit sy set =
// section sy visible). Filled by occlusion_bfs, consumed by the draw path
// and the --bench cull harness.
static_assert(kSectionsPerChunk <= 8, "section reach mask is a uint8_t");
using SectionReachableMap =
    std::unordered_map<ChunkCoord, std::uint8_t, ChunkCoordHash>;

// Breadth-first traversal of the section visibility graph, seeded at the
// camera's section. A section is marked reachable when a sightline could
// get there: each BFS step must (a) stay inside the frustum (full section
// box test), (b) pass through the source section's air (face-pair
// connectivity from compute_section_visibility), and (c) never reverse a
// direction already taken on the path (Minecraft's cave-culling rule, which
// stops wrap-around false positives). visibility_of returns a chunk's masks
// or nullptr for unloaded chunks. Returns false without marking anything
// when the camera's own chunk isn't loaded - callers fall back to
// frustum-only culling.
bool occlusion_bfs(
    const glm::vec3& camera_pos,
    const gfx::Frustum& frustum,
    const std::function<const SectionVisArray*(ChunkCoord)>& visibility_of,
    SectionReachableMap& reachable);

// True if section sy (tight mesh AABB `aabb`) should draw given its chunk's
// reachable mask. Encodes the upward-spill rule (greedy quads bucket by
// bottom Y, so a section's AABB can span slabs above it); shared between the
// renderer and the --bench cull harness so they can't drift apart.
bool section_reachable_in_mask(std::uint8_t mask, int sy, const gfx::AABB& aabb);

class World {
public:
    using ColumnFiller = std::function<void(int world_x, int world_z,
                                            Chunk& c, int local_x, int local_z)>;
    void generate_grid(int radius, const ColumnFiller& fill_column);

    struct GenStats {
        int chunks_generated = 0;
        double gen_ms = 0.0;
        double mesh_ms = 0.0;
        double total_ms = 0.0;
    };
    GenStats generate_grid(int radius, const TerrainGen& terrain);

    void enqueue_grid_async(int radius, const TerrainGen& terrain, core::ThreadPool& pool);

    struct StreamStats {
        int evicted = 0;
        int requested = 0;
        // Edit persistence, whole-chunk path: chunks RLE-stashed on
        // eviction and rebuilt from the stash on re-entry.
        //
        // Only chunks that came off disk take this path now. A player's
        // edits ride back in through slice_edits_, replayed over
        // regenerated terrain, which is what lets an edited chunk keep
        // changing with the slice instead of freezing.
        int stashed = 0;
        int restored = 0;
        // Chunks that came back carrying replayed player edits.
        int replayed = 0;
    };
    StreamStats update_streaming(ChunkCoord center, int radius,
                                 const TerrainGen& terrain,
                                 core::ThreadPool& pool);

    int  drain_finished(int max_per_frame = 8);
    int  pending_async() const;

    // Submits a worker job that greedy-meshes the already-decoded chunk and
    // pushes the result onto the finished queue. The caller drains via
    // drain_finished on the main (GL) thread. Used by load_world to
    // parallelize meshing during F6 / --bench-io load - mirrors the
    // enqueue_grid_async path but skips terrain.fill_chunk.
    // preserve_on_evict: true when the chunk cannot be regenerated from
    // the active terrain (player edits, or a save whose seed is unknown
    // or different); such chunks stash on eviction instead of vanishing.
    // Mesh an already-decoded chunk off-thread: a stash restore, or a
    // re-mesh of a chunk whose neighbour just landed.
    //
    // `stamp` is which slice the resulting chunk belongs to, and it is a
    // required argument rather than a default because getting it wrong is
    // invisible. It used to be omitted entirely, so every chunk down this
    // path was stamped with FinishedChunk's default (w=0, theta=0). At
    // any nonzero slice that chunk was instantly stale again, got
    // re-issued, landed stamped 0 again, and the world never converged -
    // one scroll notch put the whole window into a rebuild loop that
    // never ended. It was invisible at w=0, theta=0, where the default
    // happens to be correct, and that is the only state the audit ran in.
    //
    // A restore takes the CURRENT slice: the stash is keyed by slice, so
    // a restored chunk belongs where it is being restored to. A re-mesh
    // takes the slot's OWN slice, because re-meshing does not regenerate
    // anything - claiming the current slice there would mark a stale
    // chunk fresh and it would stop being rebuilt.
    void enqueue_decoded_chunk(ChunkCoord c, Chunk chunk, core::ThreadPool& pool,
                               bool preserve_on_evict,
                               TerrainGen4D::Slice stamp,
                               bool from_disk = false);
    void request_terrain_chunk(ChunkCoord c, const TerrainGen& terrain,
                               core::ThreadPool& pool);

    // Drops every chunk + pending request. Intended for full-world reload
    // (save/load); does not cancel in-flight worker jobs but their results
    // get discarded in drain_finished().
    void clear_all();

    // Iterates every loaded chunk slot in unspecified order. Read-only.
    void for_each_chunk(
        const std::function<void(ChunkCoord, const Chunk&)>& fn) const;

    // ----- the fourth dimension ----------------------------------------
    //
    // Opt-in and entirely additive: with no slice source set, every path
    // below behaves exactly as it did before and the engine is the 3D
    // engine. Setting one swaps which generator the worker jobs call.
    //
    // A 4D world reaches a 3D renderer by slicing - the mesher, the
    // culler, the lighting and the renderer never learn there is a fourth
    // axis, because they only ever see the slice at the player's w. That
    // is what keeps this a change to generation and streaming rather than
    // to everything.
    // Point the world at a 4D generator and put it on a named slice.
    //
    // A full reset, orientation included: this says "start here", and a
    // caller that means it does not want the previous slice's tilt or
    // z origin surviving. --bench-4d resets between phases and had to
    // clear those two separately before this did it.
    void set_slice_source(const TerrainGen4D* gen, float w) {
        slice_gen_ = gen;
        slice_w_ = w;
        travel_w_ = w;
        meshed_w_ = w;
        slice_theta_ = 0.0f;
        slice_z_shift_ = 0.0f;
        slice_phi_ = 0.0f;
        slice_x_shift_ = 0.0f;
    }
    bool  is_4d() const { return slice_gen_ != nullptr; }
    float slice_w() const { return slice_w_; }
    // Which integer slice edits made right now belong to. Always 0 in the
    // 3D engine, so the stash keys are exactly what they always were.
    //
    // Derived from travel_w_, which only TRAVEL changes - never from
    // slice_w_, which rotation rewrites every notch.
    //
    // Rotating about the player leaves the player's 4D position exactly
    // where it was, so it must not move them to a different edit
    // namespace. It did. slice_w_ comes back from a round trip as a tiny
    // residue rather than as zero, and floor is one-sided there, so a
    // value of -1e-16 reads as slice -1 while every resident chunk still
    // records slice 0. Seven notches out and back, fifty blocks from
    // spawn - somebody trying the wheel - was enough. The lookup then
    // misses, the terrain regenerates without the edit, and the edit is
    // filed under a key the player will never consult again. Scrolling
    // back does not bring it back.
    //
    // Rounding instead of flooring does not help; it moves the straddle
    // to +/-0.5 and waits.
    std::int32_t edit_slice() const {
        return slice_gen_ ? static_cast<std::int32_t>(std::floor(travel_w_)) : 0;
    }
    // The w of the most-stale chunk anywhere in the window. Informational:
    // at a large radius the worst chunk is past the fog, so this says more
    // about window size than about what the player sees.
    float meshed_w() const { return meshed_w_; }

    // The w of the most-stale chunk within `chunk_radius` of the camera -
    // the freshness of the world the player is actually looking at.
    //
    // This is the number worth gating on. Judging by the global worst
    // makes a large draw distance look broken: at radius 12 the far corner
    // is nearly two hundred blocks out, behind fog, and its being a few
    // hundredths of a w behind is invisible and harmless. The near field
    // is what the eye checks.
    float near_meshed_w(int chunk_radius = 4) const;

    // How many resident chunks are behind the current slice, and how many
    // there are. The honest "is the world keeping up" measure for BOTH
    // motions through w.
    //
    // near_meshed_w cannot do that job for a rotation: rotating does not
    // change slice_w, so a w-based lag is zero by construction whatever
    // the geometry is actually doing. This counts staleness the way
    // stream_slice decides it, so it means the same thing on either axis.
    // One block a player changed, as an offset into the chunk and what
    // they changed it to.
    //
    // Edits are kept as a REPLAY LIST rather than as a snapshot of the
    // chunk they belong to, and that is the whole point. The stash used
    // to hold the entire chunk and restore it verbatim, so an edited
    // chunk stopped being generated at all - and once the slice could
    // rotate, that meant a single placed block froze its whole 16x256x16
    // chunk against every further turn of the wheel. Measured: the edited
    // chunk's surface stayed at 29 through sixty notches while its
    // unedited neighbour moved 45 -> 41. A seam in the world, produced by
    // building in it.
    //
    // A replay list regenerates the terrain for whatever slice is current
    // and puts the edits back on top, so a built structure turns with the
    // world instead of pinning a hole in it.
    struct VoxelEdit {
        std::uint32_t index;   // ((y * kChunkSizeZ) + z) * kChunkSizeX + x
        std::uint8_t  block;
    };

    struct SliceLag { int stale; int resident; };
    SliceLag slice_lag() const;

    // How far a chunk's terrain has moved in the noise field between the
    // slice it was generated on and the current one - a distance, in the
    // same units for both motions, rather than a weighted sum of two
    // numbers that are not commensurable.
    //
    // This replaces `|dw| + |dtheta| * 32`. That form had to invent a
    // constant to convert an angle into a length, and 32 was chosen to
    // make the first scroll notch clear the threshold rather than to
    // describe anything. It was wrong in two directions at once: too
    // eager for chunks near the axis, which it marked stale for a
    // displacement of nearly zero, and too timid for the far ones.
    //
    // A displacement is the honest quantity, and it only became a valid
    // proxy for how much the terrain CHANGES once the noise space was
    // made isotropic - before that, moving a given distance along w
    // changed the world six times more than moving it along z, so no
    // single threshold could have been right for both. Now equal
    // displacement means equal expected change whatever direction it is
    // in, which is exactly what a staleness test needs.
    float slice_drift(ChunkCoord c, TerrainGen4D::Slice from) const;

    // Target interval between rebuilds while travelling along w, in
    // seconds. The threshold is derived from this and the player's speed
    // rather than fixed, so the rebuild rate does not scale with speed.
    //
    // Deliberately shorter than any radius can actually sustain, because
    // it is not the rate limiter - resample_slice is. That declines while
    // a rebuild is draining, so the real cadence is whatever the worker
    // pool manages, and this only has to avoid being the binding
    // constraint. The two interact well because a rebuild always targets
    // the player's CURRENT w rather than the next increment: however long
    // the throttle makes you wait, the rebuild you get catches all the
    // way up.
    //
    // The effect is that the engine tunes itself to the draw distance. At
    // radius 4 (81 chunks, ~0.04 s a rebuild) it updates about twenty
    // times a second and the terrain genuinely flows; at radius 12 (625
    // chunks, ~0.28 s) the throttle holds it near three, and each update
    // is correspondingly larger. Neither needs a different constant.
    static constexpr float kSliceRebuildPeriod = 0.05f;

    // Floor and ceiling on the derived threshold. The floor stops a
    // near-zero speed from rebuilding every frame; the ceiling stops a
    // very large one from jumping the world somewhere unrecognisable in
    // a single update.
    static constexpr float kSliceStepMin = 0.015f;
    // The same threshold expressed as a distance in the noise field,
    // which is what slice_drift returns. kSliceStepMin is a w offset and
    // one unit of w is kWScale units of noise, so this is the identical
    // sensitivity to travel that the engine has always had - a pure
    // translation of kSliceStepMin displaces a chunk by exactly this
    // much - now stated in units a rotation can also be measured in.
    static constexpr float kSliceDriftMin = kSliceStepMin * kWScale;
    static constexpr float kSliceStepMax = 0.60f;

    // How far the player can travel along w before the world is rebuilt
    // for the new position.
    //
    // Not zero, because a rebuild is not free: every chunk in the window
    // changes when w moves (the cost model in docs/4d.md measured 100%),
    // so re-meshing on every frame of movement is not affordable. Not
    // large either, or the fourth axis goes back to being a menu of
    // discrete worlds.
    //
    // 0.12 is about eight rebuilds per world-unit of travel. At a w speed
    // of ~1.5 units/sec that is a rebuild every ~0.09 s, and each one is
    // spread across the worker pool and drained a few chunks per frame,
    // so the terrain ripples into its new shape instead of stalling.
    static constexpr float kSliceRemeshStep = 0.12f;

    // How fast the player travels along w, in world units per second.
    //
    // Bounded by what the worker pool can rebuild, not by feel. A rebuild
    // re-requests every chunk in the window - 625 at radius 12 - and the
    // pool sustains about 2,200 chunks/sec, so a rebuild costs ~0.28 s
    // there. At a threshold of 0.12 that allows roughly 0.43 units/sec
    // before the world stops keeping up with the player.
    //
    // 0.4 sits just inside that. One world unit of w changes about half
    // the columns, so this crosses a visibly different world every two
    // and a half seconds - a pace for exploring an axis rather than
    // flicking through it. Sprint triples it and does outrun the pool at
    // radius 12, which is the honest trade: hold shift and the terrain
    // lags behind you.
    static constexpr float kWalkSpeedW   = 0.4f;
    static constexpr float kSprintSpeedW = 1.2f;

    // Moves the player's w by `delta` and rebuilds the world if that has
    // taken it far enough from the geometry's w to matter. Returns how
    // many chunks were re-requested, which is 0 on most calls.
    //
    // Chunks are NOT cleared: the old geometry keeps drawing until its
    // replacement lands, so travelling along w morphs the world rather
    // than blinking it.
    // `speed` is the player's current w speed in units/sec, used only to
    // derive the rebuild threshold; pass 0 to fall back to kSliceRemeshStep.
    // Moves the player along w and nothing else. The world catches up
    // through stream_slice, which the render loop calls every frame.
    void advance_w(float delta) {
        if (!slice_gen_) return;
        slice_w_ += delta;
        travel_w_ += delta;
    }

    // Rotates the slicing hyperplane in the (z, w) plane.
    //
    // This is the control 4D Miner puts on the scroll wheel, and it is
    // what translation alone cannot do: a tilted cut meets the 4D lattice
    // at an angle, so structures present a different cross-section and
    // appear to change shape. Translation only ever swaps one axis-aligned
    // world for another.
    // Turn the slice about the PLAYER, not about the world origin.
    //
    // Rotating about the origin makes the wheel's effect depend on where
    // you are standing, because a tilt displaces a point in proportion to
    // its distance from the axis it turns about. Measured, one notch:
    //
    //     player z     columns changed
    //            0       7.0%  max  1     <- spawn: almost nothing
    //          512      62.7%  max  4     <- 100 seconds of walking
    //         8192      95.8%  max 26
    //
    // and at z = 0 exactly the row is invariant: no tilt, at any angle,
    // ever changes it. Standing where the engine spawns you and looking
    // down +/-x, the wheel does nothing at all. That is not a tuning
    // problem, it is the pivot being in the wrong place.
    //
    // Turning about the player instead is a change of which point on the
    // slice stays fixed, so it is still the same family of hyperplanes.
    // Keeping the player's own 4D position fixed while theta moves means
    // the offset has to move with it:
    //
    //     o' = o cos(dtheta) - pz sin(dtheta)
    //
    // where o = kWScale * w is the slice's perpendicular offset and pz is
    // the player's z. The terrain under the player's feet then stays put
    // and the world turns around them, which is what a player expects
    // from a control that is advertised as rotating their view of 4D.
    void rotate_slice(float delta, float player_z) {
        if (!slice_gen_) return;
        // Turn the pair (offset, player-position-along-the-slice) as a
        // vector. That is all rotating about the player is: the player's
        // 4D position is fixed, so its coordinates in the slice's own
        // frame rotate with the frame.
        //
        //     u  = player_z + z_shift      (the player's slice-z)
        //     o' = o cos d - u sin d
        //     u' = o sin d + u cos d
        //
        // and the new shift is whatever puts the player back at their own
        // world z. Doing it as a rotation of a pair is what makes it
        // exactly reversible - a rotation by -d undoes a rotation by d,
        // for any offset and any player position. The earlier version
        // updated the offset alone and lost the u term, so a notch out
        // and back left o at o*cos^2(d): standing still and scrolling to
        // and fro slid the player along w, 8.5% of their w after ten
        // thousand notches, with the HUD's counter drifting to match.
        const float c = std::cos(delta), sn = std::sin(delta);
        const float o = slice_w_ * kWScale;
        const float u = player_z + slice_z_shift_;
        const float o2 = o * c - u * sn;
        const float u2 = o * sn + u * c;
        slice_w_ = o2 / kWScale;
        slice_z_shift_ = u2 - player_z;
        slice_theta_ += delta;
        // Kept in [-pi, pi]. A rotation is 2pi-periodic and the sampling
        // goes through sin and cos, so this is exactly the identity - the
        // same slice, named by the angle a reader would name it.
        //
        // Not fixing an observed bug, and worth saying so: a float loses
        // the resolution of one notch somewhere past 1e5 radians, which is
        // six days of continuous flicking, and --slice-tilt is clamped to
        // +/-3.2 so the command line cannot get there either. It is here
        // because an unbounded accumulator is a bad thing to leave lying
        // around, and because the HUD reads better showing an orientation
        // than a running total.
        //
        // It is only safe because staleness is a displacement now. The old
        // |slice_theta_ - slot.slice_theta| would have seen a wrap as a
        // 2pi jump and marked the entire world stale at the crossing;
        // slice_drift goes through to_4d, which is periodic, so a wrap is
        // invisible to it.
        constexpr float kTwoPi = 6.28318530718f;
        if (slice_theta_ >  kTwoPi * 0.5f) slice_theta_ -= kTwoPi;
        if (slice_theta_ < -kTwoPi * 0.5f) slice_theta_ += kTwoPi;
    }
    float slice_theta() const { return slice_theta_; }
    float slice_phi() const { return slice_phi_; }

    // Turn the cut in the XW plane, about the player.
    //
    // The same construction as rotate_slice, one plane over: the pair
    // (offset, player-slice-x) rotates as a vector, and x_shift puts the
    // player back at their own world x so the turn is exactly reversible
    // and leaves the ground under them alone.
    //
    // Two planes rather than one because a single plane cannot reach a
    // whole axis of 4D orientation - you can lean the world away from you
    // but never sideways. 4D Miner splits them across the two mouse axes
    // for the same reason.
    void rotate_slice_xw(float delta, float player_x) {
        if (!slice_gen_) return;
        const float c = std::cos(delta), sn = std::sin(delta);
        const float o = slice_w_ * kWScale;
        const float u = player_x + slice_x_shift_;
        const float o2 = o * c - u * sn;
        const float u2 = o * sn + u * c;
        slice_w_ = o2 / kWScale;
        slice_x_shift_ = u2 - player_x;
        slice_phi_ += delta;
        constexpr float kTwoPi = 6.28318530718f;
        if (slice_phi_ >  kTwoPi * 0.5f) slice_phi_ -= kTwoPi;
        if (slice_phi_ < -kTwoPi * 0.5f) slice_phi_ += kTwoPi;
    }

    TerrainGen4D::Slice slice() const {
        return {slice_w_, slice_theta_, slice_z_shift_,
                slice_phi_, slice_x_shift_};
    }

    // Legacy one-shot: move and, if that crossed the threshold, rebuild
    // the whole window synchronously. Kept for --verify-4d, which wants a
    // definite before and after.
    int move_w(float delta, float speed, const TerrainGen& terrain,
               core::ThreadPool& pool);

    // Rebuilds at the current w regardless of how far it has drifted.
    // Whole-window and synchronous to request; used by --verify-4d, which
    // wants a definite before and after rather than a rolling update.
    int resample_slice(const TerrainGen& terrain, core::ThreadPool& pool);

    // The continuous path, called every frame while the player travels.
    //
    // Re-requests up to `budget` chunks whose own slice_w has drifted
    // furthest from the player's, nearest to the camera first. Returns how
    // many were re-requested, which is 0 when nothing has drifted far
    // enough to matter.
    //
    // This replaces waiting for a whole-window rebuild. The world is never
    // globally stale or globally fresh; it is a field that the workers are
    // continuously pulling toward the player's w, and the budget is what
    // keeps that inside a frame.
    int stream_slice(const TerrainGen& terrain, core::ThreadPool& pool,
                     int budget = 24);

    BlockId block_at(int wx, int wy, int wz) const;
    bool    set_block(int wx, int wy, int wz, BlockId b);

    struct RayHit {
        bool  hit = false;
        int   block_x = 0, block_y = 0, block_z = 0;
        int   nx = 0, ny = 0, nz = 0;  // face normal (one of -1/0/+1)
        float distance = 0.0f;
    };
    RayHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                   float max_distance = 8.0f) const;

    // See only_chunk_ below. nullopt draws everything, which is the
    // default and the only state --validate and the benches ever see.
    void set_only_chunk(std::optional<ChunkCoord> c) { only_chunk_ = c; }
    std::optional<ChunkCoord> only_chunk() const { return only_chunk_; }

    DrawStats draw_visible(const gfx::Frustum& frustum, const gfx::Shader& shader) const;
    DrawStats draw_visible_with(const gfx::Frustum& frustum,
        std::function<void(const glm::mat4& model)> set_model) const;

    // draw_visible plus occlusion: only sections the camera can reach
    // through air (occlusion_bfs) are drawn. Falls back to plain
    // draw_visible when the camera's chunk isn't loaded. The shadow pass
    // must NOT use this - its frustum belongs to the light, not the camera.
    DrawStats draw_visible_occluded(const gfx::Frustum& frustum,
                                    const glm::vec3& camera_pos,
                                    const gfx::Shader& shader) const;

    void debug_dump_visibility(const gfx::Frustum& frustum) const;

    // Reads every chunk's VBO/EBO back off the GPU and checks each triangle
    // is an axis-aligned face backed by a solid block in that chunk's data.
    // Prints offenders; returns their count. Diagnostic for phantom-geometry
    // bugs - validates what the GPU draws, not what the CPU built.
    int debug_validate_gpu_meshes() const;

    // Which mesher builds every chunk from here on. Set once before the
    // world is generated; each job captures it by value at submit time, so
    // changing it mid-flight cannot tear a job in half.
    void set_mesher(MesherKind kind) { mesher_kind_ = kind; }
    MesherKind mesher() const { return mesher_kind_; }

    // What the meshes currently resident are encoded in. The draw path
    // folds xz into u_model and hands uv to the shader; both are 1 unless
    // the prism mesher is running, which keeps every cube-path draw call
    // byte-for-byte what it was.
    float mesh_xz_scale() const { return mesh_xz_scale_; }
    float mesh_uv_scale() const { return mesh_uv_scale_; }

    std::size_t chunk_count() const { return chunks_.size(); }
    // Chunks still owed a re-mesh because a neighbour landed after them.
    // A world with a nonzero count draws correctly but is still carrying
    // boundary faces its neighbours hide, so anything measuring the
    // resident footprint should wait for this to reach zero.
    std::size_t pending_remesh() const { return dirty_meshes_.size(); }

    // Issues up to max_jobs re-mesh jobs for chunks whose neighbours
    // landed after they were meshed. Call every frame: this is driven by
    // chunks arriving, not by the camera moving, so it must not hang off
    // update_streaming - which only runs when the player crosses a chunk
    // boundary and would leave a static camera never converging.
    //
    // Bounded per call so a settling world spreads the work over frames.
    // Chunks on this queue draw correctly, they just draw faces their
    // neighbour now hides, so being late costs nothing but bandwidth.
    int flush_pending_remeshes(core::ThreadPool& pool, int max_jobs = 4);
    bool has_chunk(ChunkCoord c) const { return chunks_.count(c) != 0; }

    // Whether the resident chunk at c is preserve-marked (player edits or
    // a non-regenerable load); this is what the save format's edited bit
    // records so a later load keeps preserving it.
    bool chunk_is_preserved(ChunkCoord c) const {
        auto it = chunks_.find(c);
        return it != chunks_.end() && it->second->player_modified;
    }

    // Edited chunks currently held only as an RLE stash (evicted from the
    // resident set but preserved). Save must include these or edits made
    // outside the current stream window would vanish from the save file.
    std::size_t stash_count() const { return edited_stash_.size(); }
    // Total RLE bytes the stash holds; O(entries), cheap at edit counts.
    std::size_t stash_bytes() const {
        std::size_t total = 0;
        for (const auto& kv : edited_stash_) total += kv.second.size();
        return total;
    }
    void for_each_stashed(
        const std::function<void(ChunkCoord,
                                 const std::vector<std::uint8_t>&)>& fn) const {
        for (const auto& kv : edited_stash_) fn(kv.first.c, kv.second);
        // Edited chunks the window evicted. Both maps are keyed by slice,
        // and a coord in both saves whichever comes first - they cannot
        // disagree, because a chunk is either generator-reproducible or
        // it is not.
        for (const auto& kv : evicted_snapshots_) fn(kv.first.c, kv.second);
    }

    // Total bytes the resident chunks hold in GPU buffers: per-chunk vertex
    // buffers plus the one shared quad index buffer. The engine's GPU mesh
    // footprint, to sit alongside RSS. O(resident chunks), so call it once
    // per report, not per drawn section.
    std::size_t resident_gpu_bytes() const {
        std::size_t total = quad_ibo_.bytes();
        for (const auto& [coord, slot] : chunks_) total += slot->gpu_bytes;
        return total;
    }

    // Cumulative timing counters across all completed chunks. Worker total
    // is wall time spent in terrain.fill_chunk + greedy meshing on a worker
    // thread (so 9 workers in parallel see this number race ahead of wall
    // clock); it splits cleanly into the terrain and mesh sub-totals.
    // Upload total is wall time spent in apply_sections on the main thread
    // (GL is single-threaded so this is a real serialization point).
    double total_worker_ms()  const { return total_worker_ms_; }
    double total_terrain_ms() const { return total_terrain_ms_; }
    double total_mesh_ms()    const { return total_mesh_ms_; }
    double total_upload_ms()  const { return total_upload_ms_; }

    // Block-edit remesh latency: set_block runs the full greedy remesh +
    // section re-bucket + GL re-upload + visibility recompute synchronously
    // on the main thread, so this is the real place/break-to-visible cost
    // the player pays. Counted only for edits that changed a block.
    std::uint64_t edit_count()   const { return edit_count_; }
    double        edit_last_ms() const { return edit_last_ms_; }
    double        edit_max_ms()  const { return edit_max_ms_; }
    double edit_avg_ms() const {
        return edit_count_ > 0
            ? edit_total_ms_ / static_cast<double>(edit_count_) : 0.0;
    }

private:
    // Neighbour identity bits for ChunkSlot::meshed_with.
    static constexpr std::uint8_t kNeighborNegX = 1 << 0;
    static constexpr std::uint8_t kNeighborPosX = 1 << 1;
    static constexpr std::uint8_t kNeighborNegZ = 1 << 2;
    static constexpr std::uint8_t kNeighborPosZ = 1 << 3;

    struct FinishedChunk {
        ChunkCoord      coord;
        Chunk           chunk;
        ChunkMeshData   mesh_data;
        SectionVisArray visibility{};
        double          worker_ms  = 0.0;
        double          terrain_ms = 0.0;
        std::uint64_t   generation = 0;  // job's world generation at submit
        // Which outstanding request this job answers. A coord can have
        // several jobs alive at once (request, evict, re-request while the
        // first job sits in the pool backlog); drain only accepts the job
        // whose stamp matches the coord's current entry in requested_.
        std::uint64_t   request_stamp = 0;
        // The w the worker generated this chunk at, carried back so the
        // slot records what it actually holds rather than what the player
        // has since moved to.
        float           slice_w = 0.0f;
        float           slice_theta = 0.0f;
        float           slice_z_shift = 0.0f;
        float           slice_phi = 0.0f;
        float           slice_x_shift = 0.0f;
        bool            from_disk = false;
        // True when the chunk must never be regenerated from terrain
        // (player edits, stash restores, or disk chunks the active seed
        // cannot reproduce); the built slot is marked player_modified so
        // eviction preserves it.
        bool            preserve_on_evict = false;
        // Which neighbours the worker actually meshed against.
        std::uint8_t    neighbor_mask = 0;
        LightGrid       light;
    };

    // Copies the four boundary layers out of whatever neighbours are
    // resident right now. Main thread only - it reads chunks_ - and the
    // result is a snapshot, so a worker holding it is unaffected by
    // anything the main thread does to those chunks afterwards.
    NeighborPlanes neighbor_planes_for(ChunkCoord c,
                                       std::uint8_t* out_mask) const;
    // The four neighbours' boundary light, copied for the same reason the
    // block planes are: a worker must not read the chunk map.
    NeighborLight neighbor_light_for(ChunkCoord c) const;

    // Chunks whose mesh predates one of their neighbours. Drained a few at
    // a time so a settling world does not spike a frame.
    std::vector<ChunkCoord> dirty_meshes_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> dirty_set_;
    void mark_neighbors_dirty(ChunkCoord c);
    void queue_remesh(ChunkCoord c);

    // Shared draw loop. reachable == nullptr means frustum-only; otherwise
    // a section draws only if some section its AABB vertically spans is in
    // the reachable mask (greedy quads bucket by their bottom Y, so a tall
    // side face can live in a lower section than the camera sees).
    // Draw only this chunk, when set. A capture aid, not a culling stage:
    // the greedy mesher's merged rectangles are the engine's headline
    // claim and a wireframe of 200 drawn chunks is an unreadable thicket
    // of edges, so the documentation shot needs one chunk in isolation.
    //
    // Deliberately not part of the frustum/occlusion pipeline. Those
    // decide what the camera can see and are gated on that being correct;
    // this one throws away geometry the camera CAN see, on purpose, which
    // is the opposite kind of thing and must not be confusable with them
    // in --validate or in a bench.
    std::optional<ChunkCoord> only_chunk_;

    DrawStats draw_impl(const gfx::Frustum& frustum,
                        const SectionReachableMap* reachable,
                        const std::function<void(const glm::mat4&)>& set_model) const;

    MesherKind mesher_kind_ = MesherKind::Greedy;
    float mesh_xz_scale_ = 1.0f;
    float mesh_uv_scale_ = 1.0f;
    std::unordered_map<ChunkCoord, std::unique_ptr<ChunkSlot>, ChunkCoordHash> chunks_;
    // The one element buffer every chunk mesh shares (all quads use the
    // same index pattern); grown to the largest chunk seen, uploaded once.
    gfx::QuadIndexBuffer quad_ibo_;
    // Scratch for draw_impl's coord-sorted traversal (see the comment
    // there); a member so the allocation is reused across passes. Mutable
    // scratch only -- draw_impl stays logically const.
    mutable std::vector<ChunkSlot*> draw_order_;
    // Outstanding chunk requests, keyed by coord with the stamp of the
    // job expected to answer. The stamp is what gives requests identity:
    // without it, a stale in-flight job (queued, evicted, re-requested)
    // could land in place of a newer one - in the worst case pristine
    // terrain draining over a stash restore, silently reverting a player
    // edit and unmarking the slot so the edit never re-stashes.
    std::unordered_map<ChunkCoord, std::uint64_t, ChunkCoordHash> requested_;
    std::uint64_t request_seq_ = 0;  // main-thread only, like generation_
    // Preserve-marked chunks that streaming evicted, kept as RLE bytes
    // (~KBs each at the measured ~58x ratio). An entry outlives its
    // restore on purpose: a restore job can itself be evicted mid-flight,
    // and the surviving entry is then the only copy of the edit. Eviction
    // of a preserved chunk re-encodes and overwrites the entry. Grows
    // with the number of distinct preserved chunks: edited ones, plus
    // loaded ones only when the save's manifest seed is absent or does
    // not match the active terrain (then nothing on disk is regenerable).
    std::unordered_map<SliceCoord, std::vector<std::uint8_t>, SliceCoordHash>
        edited_stash_;

    mutable std::mutex                 finished_mutex_;
    std::queue<FinishedChunk>          finished_;
    std::atomic<int>                   jobs_in_flight_{0};
    // Bumped whenever the resident set is wiped (clear_all, enqueue_grid_async);
    // a finished job whose stamp no longer matches is discarded, so a load
    // cannot pick up regenerated terrain from a job the wipe outran. Only
    // touched on the main thread (submit, drain, wipe), so not atomic.
    std::uint64_t                      generation_ = 0;

    // Null in the 3D engine, which is the default and the only state the
    // benches and --validate ever see.
    const TerrainGen4D* slice_gen_ = nullptr;
    float               slice_w_ = 0.0f;      // where the player is
    // How far the player has TRAVELLED along w, which is what the
    // edit namespace is keyed on. Rotation deliberately leaves it
    // alone: turning the slice does not move the player.
    float               travel_w_ = 0.0f;
    float               slice_theta_ = 0.0f;  // how their cut is tilted
    // Where the slice's z axis starts; moves only when the cut turns.
    float               slice_z_shift_ = 0.0f;
    // The XW plane, and the shift that pivots it at the player.
    float               slice_phi_ = 0.0f;
    float               slice_x_shift_ = 0.0f;
    // Player edits, by chunk and slice, replayed over freshly generated
    // terrain. See VoxelEdit.
    std::unordered_map<SliceCoord, std::vector<VoxelEdit>, SliceCoordHash>
                        slice_edits_;
    // Counts chunk jobs that carried replayed edits, so update_streaming
    // can report how many came back that way.
    int                 stream_replayed_ = 0;
    // Whole-chunk snapshots of edited chunks the stream window evicted,
    // kept ONLY so a save can write them.
    //
    // Separate from edited_stash_ on purpose. The restore paths consult
    // edited_stash_ and hand a chunk back verbatim, which is right for a
    // chunk the generator cannot reproduce and wrong for one it can - a
    // reproducible chunk handed back whole stops being generated, and
    // then stops turning when the 4D slice does. These entries are never
    // restored from; the chunk comes back through the generator with its
    // edits replayed. They exist because save_world writes bytes, and an
    // evicted chunk has none until somebody makes them.
    //
    // Without this, an edit made out of view was lost by any save: dig a
    // hole, walk past the stream radius, save, reload, and the hole is
    // gone. In-session it looked fine, because the replay list restored
    // it on re-entry.
    std::unordered_map<SliceCoord, std::vector<std::uint8_t>, SliceCoordHash>
                        evicted_snapshots_;
    float               meshed_w_ = 0.0f;  // where the geometry is
    // The integer slice the resident chunks belong to. Needed separately
    // from edit_slice() because a rebuild has to stash the OLD slice's
    // edits before adopting the new one.
    std::int32_t        meshed_slice_ = 0;
    // Where the player was standing at the last streaming update, so a
    // rebuild can start with the chunks they are looking at.
    ChunkCoord          last_center_{0, 0};
    double                             total_worker_ms_  = 0.0;
    double                             total_terrain_ms_ = 0.0;
    double                             total_mesh_ms_    = 0.0;
    double                             total_upload_ms_  = 0.0;
    std::uint64_t                      edit_count_    = 0;
    double                             edit_last_ms_  = 0.0;
    double                             edit_max_ms_   = 0.0;
    double                             edit_total_ms_ = 0.0;
};

}  // namespace world
