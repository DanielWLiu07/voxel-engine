#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "bench/frame_report.h"
#include "bench/mesher_bench.h"
#include "core/cli_options.h"
#include "core/cpu_time.h"
#include "core/frame_stats.h"
#include "core/capture_mode.h"
#include "core/input.h"
#include "core/key_bindings.h"
#include "core/profiler.h"
#include "core/thread_pool.h"
#include "core/window.h"
#include "game/player.h"
#include "gfx/camera.h"
#include "gfx/frustum.h"
#include "gfx/shader.h"
#include "gfx/cascaded_shadow_map.h"
#include "gfx/post_process.h"
#include "gfx/screenshot.h"
#include "gfx/texture_atlas.h"
#include "gfx/water.h"
#include "gfx/wireframe_cube.h"
#include "render/lighting.h"
#include "render/passes.h"
#include "render/shader_set.h"
#include "ui/debug_hud.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/terrain_gen.h"
#include "world/world.h"
#include "world/world_io.h"

#include <algorithm>
#include <numbers>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr float kFlySpeed       = 16.0f;
constexpr float kFlySprintSpeed = 60.0f;
constexpr int   kStreamRadius   = 12;
constexpr const char* kSaveDir = "./saves/world1";
constexpr float kWaterSize      = 480.0f;
constexpr int   kWaterSubdiv    = 200;
constexpr int   kShadowMapSize  = 2048;
constexpr float kShadowNear     = 0.1f;
constexpr float kShadowFar      = 250.0f;

const glm::vec3 kBlockPalette[world::kBlockPaletteSize] = {
    {1.00f, 0.00f, 1.00f},  // Air (never seen)
    {0.55f, 0.55f, 0.58f},  // Stone
    {0.50f, 0.34f, 0.20f},  // Dirt
    {0.34f, 0.62f, 0.27f},  // Grass
    {0.88f, 0.80f, 0.55f},  // Sand
    {0.42f, 0.27f, 0.13f},  // Wood
    {0.22f, 0.46f, 0.20f},  // Leaves
    {0.95f, 0.96f, 0.98f},  // Snow
    {1.00f, 0.86f, 0.55f},  // Glow (emissive)
};

// Which way the bytes went. This used to be inferred from the first
// character of the verb string - `verb[0] == 's' ? "wrote" : "read"` -
// which is correct for exactly the two words that were passed and
// silently wrong for any third: "store" prints "wrote" by luck, "restore"
// prints "read" by luck, "sync" prints "wrote" and means neither.
enum class IoDirection { Save, Load };

// Shared save/load console report: both directions measure and print
// identically, so the two lines stay comparable at a glance.
void print_io_report(IoDirection dir, int chunks, double ms,
                     std::size_t bytes_disk, std::size_t bytes_raw,
                     bool ok) {
    const char* verb = dir == IoDirection::Save ? "save" : "load";
    const char* past = dir == IoDirection::Save ? "wrote" : "read";
    const double ratio = bytes_disk > 0
        ? static_cast<double>(bytes_raw) / bytes_disk : 0.0;
    const double secs = ms / 1000.0;
    const double mb_disk = bytes_disk / (1024.0 * 1024.0);
    const double mb_raw  = bytes_raw  / (1024.0 * 1024.0);
    std::printf("[%s] %s %d chunks in %.1f ms  |  "
                "%.2f MB on disk vs %.2f MB raw  |  %.1fx ratio  |  "
                "%.0f MB/s disk, %.0f MB/s raw  |  %s\n",
                verb, past, chunks, ms,
                mb_disk, mb_raw, ratio,
                secs > 0.0 ? mb_disk / secs : 0.0,
                secs > 0.0 ? mb_raw  / secs : 0.0,
                ok ? "ok" : "ERRORS");
}

// F5 / F6. Both time the operation and report through print_io_report, so
// an interactive save and the --bench-io numbers describe the same thing.
void save_world_to_disk(world::World& wrld, std::uint32_t seed) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto s = world::save_world(wrld, kSaveDir, seed);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    print_io_report(IoDirection::Save, s.chunks_written, ms,
                    s.bytes_written, s.bytes_raw, s.ok);
}

// Skipped files are a warning rather than a failure: a corrupt chunk file
// costs that chunk, and the streaming path will regenerate it. Silently
// loading a partial world would not be obvious from the report line,
// which is why the count is printed separately from the ok flag.
void load_world_from_disk(world::World& wrld, core::ThreadPool& pool,
                          std::uint32_t seed) {
    const auto t0 = std::chrono::steady_clock::now();
    wrld.clear_all();
    const auto l = world::load_world(wrld, kSaveDir, pool, seed);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    print_io_report(IoDirection::Load, l.chunks_read, ms,
                    l.bytes_read, l.bytes_raw,
                    l.ok && l.files_skipped == 0);
    if (l.files_skipped > 0) {
        std::fprintf(stderr, "[load] WARNING: %d chunk file%s corrupt "
                     "or unreadable, skipped\n",
                     l.files_skipped, l.files_skipped == 1 ? "" : "s");
    }
}

fs::path find_asset_root(const char* argv0) {
    fs::path start = fs::absolute(argv0).parent_path();
    for (fs::path p = start; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "shaders" / "basic.vert")) return p;
        if (p == p.root_path()) break;
    }
    return fs::current_path();
}

void handle_block_interaction(core::Input& input,
                              const gfx::FlyCamera& cam,
                              const game::Player& player,
                              bool walk_mode,
                              world::World& wrld,
                              world::BlockId place_id) {
    bool break_block = input.mouse_button_pressed(GLFW_MOUSE_BUTTON_LEFT);
    bool place_block = input.mouse_button_pressed(GLFW_MOUSE_BUTTON_RIGHT);
    if (!break_block && !place_block) return;

    auto hit = wrld.raycast(cam.position(), cam.forward(), 8.0f);
    if (!hit.hit) return;

    if (break_block) {
        wrld.set_block(hit.block_x, hit.block_y, hit.block_z, world::BlockId::Air);
        return;
    }

    int px = hit.block_x + hit.nx;
    int py = hit.block_y + hit.ny;
    int pz = hit.block_z + hit.nz;

    if (walk_mode) {
        constexpr float hw = game::Player::kWidth * 0.5f;
        glm::vec3 feet = player.feet_position();
        if (px + 1 > feet.x - hw && px < feet.x + hw &&
            py + 1 > feet.y      && py < feet.y + game::Player::kHeight &&
            pz + 1 > feet.z - hw && pz < feet.z + hw) {
            return;
        }
    }
    wrld.set_block(px, py, pz, place_id);
}

const char* block_name(world::BlockId b) {
    switch (b) {
    case world::BlockId::Stone:  return "Stone";
    case world::BlockId::Dirt:   return "Dirt";
    case world::BlockId::Grass:  return "Grass";
    case world::BlockId::Sand:   return "Sand";
    case world::BlockId::Wood:   return "Wood";
    case world::BlockId::Leaves: return "Leaves";
    case world::BlockId::Snow:   return "Snow";
    case world::BlockId::Glow:   return "Glow";
    default:                     return "?";
    }
}

void update_movement(core::Input& input, float dt,
                     gfx::FlyCamera& cam, game::Player& player,
                     const world::World& wrld, bool walk_mode,
                     bool slice_look) {
    // While the 4D look modifier is held the mouse turns the CUT, not the
    // camera, so the camera must not also consume the delta - otherwise
    // the view swings while the world turns and neither reads clearly.
    if (!slice_look) {
        cam.apply_mouse_delta(input.mouse_dx(), input.mouse_dy(), 0.12f);
    }

    if (walk_mode) {
        glm::vec3 fwd = cam.forward();   fwd.y = 0.0f;
        glm::vec3 right = cam.right();   right.y = 0.0f;
        if (glm::dot(fwd, fwd) > 0.0f)     fwd = glm::normalize(fwd);
        if (glm::dot(right, right) > 0.0f) right = glm::normalize(right);

        glm::vec3 wish(0.0f);
        if (input.key_down(GLFW_KEY_W)) wish += fwd;
        if (input.key_down(GLFW_KEY_S)) wish -= fwd;
        if (input.key_down(GLFW_KEY_D)) wish += right;
        if (input.key_down(GLFW_KEY_A)) wish -= right;
        if (glm::dot(wish, wish) > 0.0f) wish = glm::normalize(wish);

        float speed = input.key_down(GLFW_KEY_LEFT_SHIFT)
            ? game::Player::kSprintSpeed : game::Player::kWalkSpeed;
        wish *= speed;

        player.update(wrld, wish, input.key_pressed(GLFW_KEY_SPACE), dt);
        cam.set_position(player.eye_position());
        return;
    }

    glm::vec3 local{0.0f};
    if (input.key_down(GLFW_KEY_W)) local.z += 1.0f;
    if (input.key_down(GLFW_KEY_S)) local.z -= 1.0f;
    if (input.key_down(GLFW_KEY_D)) local.x += 1.0f;
    if (input.key_down(GLFW_KEY_A)) local.x -= 1.0f;
    if (input.key_down(GLFW_KEY_SPACE))        local.y += 1.0f;
    if (input.key_down(GLFW_KEY_LEFT_CONTROL)) local.y -= 1.0f;
    cam.move_local(local,
                   input.key_down(GLFW_KEY_LEFT_SHIFT) ? kFlySprintSpeed : kFlySpeed,
                   dt);
}

// One point on the scripted camera orbit: a fixed-radius circle at constant
// height, always looking at the scene center. Shared by the clip capture
// and the orbit frame benchmark so both trace the identical path; `frame`
// runs 0..total-1 for one full revolution.
struct OrbitPose {
    glm::vec3 pos;
    float yaw;
    float pitch;
};
// Default orbit: the biome triple point near spawn. --orbit-center moves
// the circle (and optionally the look-at height) so clips can frame other
// set pieces - the lake at (288,-400) is the other README subject.
OrbitPose orbit_pose_at(int frame, int total,
                        glm::vec3 center = {-10.0f, 45.0f, -10.0f}) {
    constexpr float radius = 65.0f;
    constexpr float height = 62.0f;
    const float angle = 2.0f * std::numbers::pi_v<float> *
                        static_cast<float>(frame) /
                        static_cast<float>(total > 0 ? total : 1);
    const glm::vec3 pos{center.x + radius * std::cos(angle), height,
                        center.z + radius * std::sin(angle)};
    const glm::vec3 dir = glm::normalize(center - pos);
    return {pos, glm::degrees(std::atan2(dir.z, dir.x)),
            glm::degrees(std::asin(dir.y))};
}

}  // namespace

int main(int argc, char** argv) {
    int cli_exit = 0;
    const auto parsed = core::parse_cli(argc, argv, kStreamRadius, cli_exit);
    if (!parsed) return cli_exit;
    const core::CliOptions& opt = *parsed;
    if (opt.run_mesher_bench) return bench::run_mesher_bench(kStreamRadius);

    // Unpacked into the names the rest of main already uses. Keeping the
    // read sites unchanged is the point of doing it this way: the parsing
    // moved, the thousand lines below it did not, so the diff shows a move
    // rather than a rewrite of the render loop.
    const int bench_frames = opt.bench_frames;
    const bool bench_pass_breakdown = opt.bench_pass_breakdown;
    const bool sky_overdraw         = opt.sky_overdraw;
    const bool bench_io = opt.bench_io;
    const bool bench_orbit = opt.bench_orbit;
    std::string_view bench_pose = opt.bench_pose;
    const glm::vec3 orbit_center = opt.orbit_center;
    const std::uint32_t terrain_seed = opt.terrain_seed;
    const int stream_radius = opt.stream_radius;
    int shot_after = opt.shot_after;  // capture modes retarget this
    const std::string shot_file = opt.shot_file;
    const std::string load_path = opt.load_path;
    const std::string save_path = opt.save_path;
    const bool start_wireframe = opt.start_wireframe;
    const int bench_edit = opt.bench_edit;
    const bool validate_mode = opt.validate_mode;
    const bool verify_edit_persistence = opt.verify_edit_persistence;
    const bool verify_4d = opt.verify_4d;
    const int thread_override = opt.thread_override;
    const int orbit_frames = opt.orbit_frames;
    const int cycle_frames = opt.cycle_frames;
    const int tilt_frames  = opt.capture_tilt;
    // The two capture questions, named once (core/capture_mode.h). Built
    // from the same values the locals above carry; shot_after is the one
    // that counts down, so `capture` is rebuilt where that matters.
    core::CaptureMode capture{shot_after, orbit_frames, cycle_frames,
                              tilt_frames,
                              bench_frames};
    const bool no_occlusion = opt.no_occlusion;
    const std::optional<world::ChunkCoord> only_chunk =
        opt.have_only_chunk
            ? std::optional<world::ChunkCoord>(world::ChunkCoord{
                  opt.only_chunk_x, opt.only_chunk_z})
            : std::nullopt;
    const glm::vec3 pose_at = opt.pose_at;
    const float pose_at_yaw = opt.pose_at_yaw;
    const float pose_at_pitch = opt.pose_at_pitch;
    const bool have_pose_at = opt.have_pose_at;


    // Declared before every GL-owning object below (shaders, world, FBOs,
    // meshes) so it destructs after them: their glDelete* calls must run
    // while the context is still current. That ordering is why this is a
    // named local at the top of main rather than something tucked inside a
    // setup helper.
    const bool headless = bench_frames > 0 || bench_io || bench_edit > 0 ||
                          validate_mode || verify_edit_persistence ||
                          verify_4d ||
                          !save_path.empty();
    bool vsync_enabled = (bench_frames == 0 && shot_after == 0);
    auto win = core::Window::create({.visible = !headless,
                                     .vsync   = vsync_enabled,
                                     .monitor = opt.monitor,
                                     .list_monitors = opt.list_monitors});
    // --list-monitors prints and stops, so a null window is success there.
    if (!win) return opt.list_monitors ? EXIT_SUCCESS : EXIT_FAILURE;
    GLFWwindow* window = win->handle();
    // Section-graph occlusion culling (O to toggle). On by default; the
    // frustum-only path stays one keypress away (or --no-occlusion) for
    // A/B comparison.
    bool occlusion_cull_enabled = !no_occlusion;
    // G toggles a wireframe terrain pass: the greedy mesher's merged faces
    // show as a few large quads where a naive mesher would draw one per block.
    bool wireframe = start_wireframe;

    std::printf("GL %d.%d  |  vendor=%s  |  renderer=%s\n",
                win->gl_version_major(), win->gl_version_minor(),
                glGetString(GL_VENDOR), glGetString(GL_RENDERER));

    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_MULTISAMPLE);

    fs::path root = find_asset_root(argv[0]);
    std::printf("[boot] asset root = %s\n", root.string().c_str());

    render::ShaderSet shaders;
    if (!shaders.load(root)) return EXIT_FAILURE;

    gfx::PostProcess postfx;
    if (!postfx.init(fb_w, fb_h)) {
        std::fprintf(stderr, "post-process init failed\n");
        return EXIT_FAILURE;
    }
    int postfx_w = fb_w, postfx_h = fb_h;
    bool postfx_failed = false;
    std::printf("[postfx] HDR %dx%d + %d-level bloom pyramid allocated\n",
                fb_w, fb_h, postfx.bloom_mip_count());

    gfx::CascadedShadowMap shadow_map;
    if (!shadow_map.init(kShadowMapSize)) {
        return EXIT_FAILURE;
    }
    std::printf("[shadow] %dx%d depth map allocated\n", kShadowMapSize, kShadowMapSize);

    gfx::WaterPlane water;
    if (!water.init(kWaterSize, kWaterSubdiv)) {
        return EXIT_FAILURE;
    }
    std::printf("[water] %.0fx%.0f plane (sea level y=%d, follows player)\n",
                kWaterSize, kWaterSize, world::kSeaLevel);

    GLuint sky_vao = 0;
    glGenVertexArrays(1, &sky_vao);

    // Procedural texture atlas for blocks. Generated once at boot.
    int ai_texture_tiles = 0;
    GLuint block_atlas = gfx::generate_block_atlas(&ai_texture_tiles);
    if (ai_texture_tiles > 0) {
        // Honesty-by-default: these tiles are AI-generated and we say so,
        // at boot and in the HUD. Provenance per file: textures/MANIFEST.toml.
        std::printf("[credit] Block textures: AI-generated (SDXL-Turbo) - "
                    "see TEXTURES.md\n");
    }
    std::printf("[atlas] %d-layer %dpx block texture array (mipmapped)\n",
                gfx::kAtlasLayers, gfx::kAtlasTilePx);
    GLuint crosshair_vao = 0;
    glGenVertexArrays(1, &crosshair_vao);

    gfx::WireframeCube selection_cube;
    selection_cube.init();

    const std::size_t worker_count = thread_override > 0
        ? static_cast<std::size_t>(thread_override)
        : std::max<std::size_t>(2, std::thread::hardware_concurrency() - 1);
    world::TerrainGen terrain(terrain_seed);
    world::World wrld;
    // Set before any chunk is generated: every mesh job captures the kind
    // at submit time.
    bool demo_lights_pending = opt.demo_lights;
    if (opt.naive_mesh) {
        wrld.set_mesher(world::MesherKind::Naive);
        std::printf("[world] naive mesher: one quad per visible face "
                    "(rendering aid, not the shipped path)\n");
    }
    // Unconditional: --only-chunk is independent of which mesher is in
    // use, and pairing it with --naive-mesh is exactly the comparison the
    // wireframe capture wants.
    wrld.set_only_chunk(only_chunk);
    if (only_chunk) {
        std::printf("[world] drawing chunk %d,%d only "
                    "(capture aid: geometry the camera can see is being "
                    "discarded on purpose)\n",
                    only_chunk->x, only_chunk->z);
    }
    // The 4D world, when asked for. Declared here so it outlives every
    // worker job that reads it, exactly as `terrain` does - the pool
    // below is destroyed before both.
    const world::TerrainGen4D terrain4d(terrain_seed);
    if (opt.four_d) {
        wrld.set_slice_source(&terrain4d, static_cast<float>(opt.slice_w));
        // --slice-tilt is applied further down, once the camera has its
        // pose, so it turns about the viewer exactly as the wheel does.
        std::printf("\n"
            "  ========================================================\n"
            "   FOUR-DIMENSIONAL WORLD   (--3d for the ordinary one)\n"
            "\n"
            "   HOLD E / Q     travel along w, the 4th axis\n"
            "   SCROLL WHEEL   rotate your 3D slice through 4D\n"
            "\n"
            "   WASD moves you through space and does NOT change the\n"
            "   world. The other two controls do, and differently:\n"
            "\n"
            "   E/Q slide you along w - the world becomes a different\n"
            "   but equally ordinary place, and Q brings it back exactly.\n"
            "\n"
            "   The WHEEL tilts the 3D slice you occupy. A tilted cut\n"
            "   meets the 4D world at an angle, so terrain shows a\n"
            "   different cross-section and structures appear to change\n"
            "   shape. This is the one that looks four-dimensional.\n"
            "\n"
            "   F2 shows w on the HUD.  Starting at w=%d.\n"
            "  ========================================================\n\n",
            opt.slice_w);
    }

    core::ThreadPool pool(worker_count);

    const int total_chunks = (2 * stream_radius + 1) * (2 * stream_radius + 1);
    std::printf("[world] streaming %d chunks (radius=%d) onto %zu workers\n",
                total_chunks, stream_radius, worker_count);

    auto async_t0 = std::chrono::steady_clock::now();
    bool loaded_from_disk = false;
    if (!load_path.empty()) {
        // Boot from a saved snapshot instead of generating: load_world reads
        // the RLE chunks (meshing them through the same worker pool), then the
        // per-frame streamer fills in anything outside the snapshot as the
        // player moves -- the same path F6 uses to swap worlds at runtime.
        auto l = world::load_world(wrld, load_path, pool, terrain_seed);
        if (l.files_skipped > 0) {
            std::fprintf(stderr,
                         "[world] WARNING: %d chunk file%s in %s corrupt or "
                         "unreadable, skipped\n",
                         l.files_skipped, l.files_skipped == 1 ? "" : "s",
                         load_path.c_str());
        }
        if (l.chunks_read > 0) {
            std::printf("[world] loaded %d chunks from %s\n",
                        l.chunks_read, load_path.c_str());
            loaded_from_disk = true;
        } else {
            std::printf("[world] --load %s had no chunks; generating instead\n",
                        load_path.c_str());
        }
    }
    if (!loaded_from_disk) {
        wrld.enqueue_grid_async(stream_radius, terrain, pool);
    }

    bool   initial_load_logged = false;
    // Every chunk resident is not the same thing as the world being
    // finished. Chunks meshed before their neighbours existed are still
    // owed a re-mesh, and anything deterministic - a screenshot, a frame
    // benchmark, a validation pass - has to wait for that queue to empty
    // or it captures a world halfway through converging, which is both
    // wrong and, worse, not reproducible.
    //
    // Kept separate from initial_load_logged on purpose: that milestone is
    // what the chunks/sec figure measures, and folding a different cost
    // into it would quietly change a published number.
    bool   world_settled = false;
    double settle_ms = 0.0;
    double initial_load_ms     = 0.0;
    world::ChunkCoord last_center{0, 0};
    int streamed_in_total  = 0;
    int streamed_out_total = 0;

    gfx::FlyCamera cam;
    // Launch vantage.
    //
    // The 3D world keeps its long-standing (0, 80, 80). The 4D world runs
    // lower - roughly y=12..56 against the 3D generator's 30..45 plus
    // lakes - so that vantage puts the camera hard against a snow face
    // that fills the frame. There is no horizon and no landmark in it, so
    // the terrain morphing along w is nearly impossible to read: the
    // fourth dimension was working and invisible, which is a worse
    // failure than it being broken.
    //
    // (0, 64, 0) looks out over coastline, islands and peaks - a view
    // with enough structure that a change to it registers.
    cam.set_position(opt.four_d ? glm::vec3{0.0f, 64.0f, 0.0f}
                                : glm::vec3{0.0f, 80.0f, 80.0f});
    cam.set_yaw_pitch(-90.0f, -35.0f);
    if (have_pose_at) {
        bench_pose = "at";
        cam.set_position(pose_at);
        cam.set_yaw_pitch(pose_at_yaw, pose_at_pitch);
    } else if (bench_frames > 0 || shot_after > 0) {
        // Named poses keep the perf table reproducible across vantage
        // points. "center" matches the --bench cull pose for direct
        // comparability with the cull-ratio table; "ground" is an
        // eye-level walk pose; "high" is a top-down vantage that
        // exercises the section-AABB cull's vertical pruning; "cave" is
        // the --bench occlusion pose inside an air pocket (seed 1337).
        if (bench_pose == "ground") {
            // Stand ON the surface: the old fixed y=35 was below the local
            // terrain height (~39), which put the camera inside the hill and
            // produced see-through "floating quad" captures.
            const float eye_y =
                static_cast<float>(terrain.height_at(0, 0)) + 2.7f;
            cam.set_position({0.0f, eye_y, 0.0f});
            cam.set_yaw_pitch(-90.0f, 0.0f);
        } else if (bench_pose == "high") {
            cam.set_position({0.0f, 150.0f, 0.0f});
            cam.set_yaw_pitch(-90.0f, -45.0f);
        } else if (bench_pose == "cave") {
            cam.set_position({-30.5f, 15.5f, -31.5f});
            cam.set_yaw_pitch(-90.0f, 0.0f);
        } else if (bench_pose == "orbit") {
            // The orbit bench drives the camera per frame; start it at the
            // path's first point so the settle happens where sampling begins.
            const OrbitPose op = orbit_pose_at(0, bench_frames, orbit_center);
            cam.set_position(op.pos);
            cam.set_yaw_pitch(op.yaw, op.pitch);
        } else {
            // default: "center"
            bench_pose = "center";
            cam.set_position({0.0f, 80.0f, 0.0f});
            cam.set_yaw_pitch(-90.0f, -15.0f);
        }
    }

    // --slice-tilt, applied here rather than where the 4D generator is
    // attached, because it has to turn about the CAMERA.
    //
    // It used to pivot at the world origin, since the camera had no pose
    // yet at that point. That made every tilted capture show more change
    // than a player sees: rotating about the origin displaces terrain in
    // proportion to its distance from z=0, so a camera parked 60 blocks
    // out watched the ground under it move, where scrolling in game
    // leaves it still and moves the distance. The stills were honest
    // about the generator and wrong about the engine.
    // In the tilt clip --slice-tilt names the sweep's AMPLITUDE rather
    // than a fixed angle, so one flag covers both "hold this cut" and
    // "swing through this much of one".
    if (opt.slice_tilt != 0.0f && tilt_frames == 0) {
        wrld.rotate_slice(opt.slice_tilt, cam.position().z);
    }

    core::Input input;
    glfwSetWindowUserPointer(window, &input);
    glfwSetScrollCallback(window, [](GLFWwindow* w, double, double dy) {
        if (auto* in = static_cast<core::Input*>(glfwGetWindowUserPointer(w))) {
            in->add_scroll(static_cast<float>(dy));
        }
    });
    input.attach(window);
    // Capture the mouse straight away for an interactive run.
    //
    // Movement is gated on the cursor being captured, and Input::attach
    // starts it released - so a fresh launch ignored WASD entirely until
    // the player happened to press Tab, with nothing on screen saying so.
    // An input trace caught this: the engine received 152 W keydowns and
    // the player never left the spawn point.
    //
    // Not for headless runs, which have no window to capture into, and
    // not for scripted captures, which lock the pose deliberately.
    if (!headless) input.set_cursor_captured(true);
    input.set_cursor_captured(true);

    ui::DebugHud hud;
    if (!hud.init(window)) {
        std::fprintf(stderr, "imgui init failed\n");
        return EXIT_FAILURE;
    }

    game::Player player;
    // The 4D world runs lower than the 3D one - its heights span roughly
    // y=12..56 against the 3D generator's 30..45-plus-lakes - so spawning
    // at the 3D height would drop the player in well above the terrain.
    player.set_position({0.0f, opt.four_d ? 64.0f : 80.0f, 0.0f});
    bool walk_mode = false;
    world::BlockId place_id = world::BlockId::Stone;

    float time_of_day = 0.35f;
    // A capture can pin the sun; interactive runs keep the default.
    if (opt.time_of_day >= 0.0f) time_of_day = opt.time_of_day;
    const float day_speed = 1.0f / 240.0f;
    // Bench/shot modes pause time-of-day so a sunrise/sunset transition
    // mid-run can't fire the shadow-resync force-refresh path (bench: timing
    // spike) or change the lighting between A/B captures. The orbit capture
    // pauses it too: constant light is what lets the last frame meet the
    // first for a seamless loop.
    bool  time_paused = capture.pins_time_of_day();
    int   capture_frame = 0;
    int   capture_settle = 0;

    core::print_bindings();

    double last_time = glfwGetTime();
    double prev_frame_time = glfwGetTime();
    int    frame_count = 0;
    uint64_t frame_index = 0;
    // Cached cascades for the stagger optimization: when a cascade is
    // skipped this frame, basic.frag must sample the existing depth layer
    // with the matrix that produced it, so the (matrix, depth) pair stays
    // locked together.
    glm::mat4 cached_light_vp[gfx::kNumCascades]{};
    float     cached_cascade_far[gfx::kNumCascades]{};
    bool      prev_shadow_active = false;
    world::DrawStats last_stats{};
    float smoothed_fps      = 0.0f;
    float smoothed_frame_ms = 0.0f;

    // --bench-frame sampling: settle countdown, per-frame wall/CPU samples,
    // triangle total, and the optional glFinish-bracketed pass timers, all
    // in one object (bench/frame_report.h) rather than eight locals used
    // 700 lines apart. Note for --pass-breakdown: the per-frame glFinish
    // stalls inflate the frame-level avg_ms heavily (~2.7x measured at
    // radius 12), so never quote avg_ms from that mode as frame time.
    bench::FrameSampler sampler(bench_frames, bench_pass_breakdown);

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = static_cast<float>(now - prev_frame_time);
        prev_frame_time = now;

        // No focus request here.
        //
        // One was tried - re-asserting glfwFocusWindow over the first few
        // frames - because a launch from a background shell was leaving
        // the terminal frontmost. It did not work, so it is not kept:
        // macOS will not activate a non-bundled process that was started
        // without a foreground session, whatever the window asks for. A
        // normal launch from the user's own shell activates fine, and the
        // request at window creation covers that.
        //
        // Keeping a fix that does not fix anything is worse than the bug,
        // because the next person to see the symptom will believe it was
        // handled.

        input.begin_frame();

        // --trace-input: make the input path observable from outside the
        // process. Prints any key the engine sees, and the player's
        // position and w whenever they move, flushed every line so a
        // watcher tailing the log sees it immediately.
        if (opt.trace_input) {
            static double last_trace = 0.0;
            static glm::vec3 last_pos{1e9f};
            static float last_w = 1e9f;
            for (int k = 32; k < 350; ++k) {
                if (input.key_down(k)) {
                    std::printf("[input] key %d down\n", k);
                    std::fflush(stdout);
                }
            }
            static bool gate_logged = false;
            if (!gate_logged) {
                std::printf("[gate] cursor_captured=%d scripted_camera=%d walk_mode=%d\n",
                            input.cursor_captured() ? 1 : 0,
                            capture.scripted_camera() ? 1 : 0, walk_mode ? 1 : 0);
                std::fflush(stdout);
                gate_logged = true;
            }
            // The CAMERA, not the player. In fly mode - the default -
            // update_movement drives cam.move_local and the player body is
            // never touched, so tracing player.feet_position() showed a
            // frozen position while the view was moving perfectly. That
            // cost a round of hunting a movement bug that did not exist.
            const glm::vec3 p = walk_mode ? player.feet_position() : cam.position();
            // near_meshed_w, not meshed_w. The global worst is a chunk
            // past the fog at any large radius, so tracing it showed
            // "geometry 0.000" while the visible world was tracking
            // within a few hundredths - the third time this trace has
            // reported the engine broken when the instrument was wrong.
            const float w = wrld.slice_w();
            if (now - last_trace > 0.25 &&
                (glm::distance(p, last_pos) > 0.01f ||
                 std::fabs(w - last_w) > 0.001f)) {
                std::printf("[state] pos %.2f,%.2f,%.2f  w %.3f (near geometry %.3f)\n",
                            p.x, p.y, p.z, w, wrld.near_meshed_w());
                std::fflush(stdout);
                last_trace = now;
                last_pos = p;
                last_w = w;
            }
        }

        smoothed_frame_ms = smoothed_frame_ms * 0.9f + (dt * 1000.0f) * 0.1f;
        float instant_fps = (dt > 0.0f) ? (1.0f / dt) : 0.0f;
        smoothed_fps = smoothed_fps * 0.9f + instant_fps * 0.1f;

        if (input.key_down(core::key_of(core::Bind::Quit)))    glfwSetWindowShouldClose(window, GLFW_TRUE);
        if (input.key_pressed(core::key_of(core::Bind::Cursor)))    input.set_cursor_captured(!input.cursor_captured());
        if (input.key_pressed(core::key_of(core::Bind::Hud)))     hud.toggle_visible();
        if (input.key_pressed(core::key_of(core::Bind::Screenshot))) {
            std::string path = gfx::save_screenshot(fb_w, fb_h);
            if (!path.empty()) std::printf("[screenshot] %s\n", path.c_str());
        }
        if (input.key_pressed(core::key_of(core::Bind::PauseTime)))      time_paused = !time_paused;
        if (input.key_pressed(core::key_of(core::Bind::Occlusion))) {
            occlusion_cull_enabled = !occlusion_cull_enabled;
            std::printf("[world] occlusion culling %s\n",
                        occlusion_cull_enabled ? "on" : "off");
        }
        // Travel along w, held rather than pressed.
        //
        // This is the difference between a fourth dimension and a menu of
        // worlds. key_down, not key_pressed: holding the key slides the
        // player through w continuously, the same way holding W slides
        // them through z, and the terrain morphs while they hold it. A
        // keypress that jumped to the next integer slice made w a
        // selector - you teleported between discrete worlds rather than
        // moving through one.
        if (wrld.is_4d()) {
            float w_axis = 0.0f;
            // --auto-w: travel forever with no input, so the fourth
            // dimension can be watched rather than driven. Reverses
            // direction every 12 units so it stays near the origin and
            // the same landscape keeps morphing back and forth, which is
            // easier to read than drifting away forever.
            if (opt.auto_w) {
                static float auto_dir = 1.0f;
                if (wrld.slice_w() > 12.0f)  auto_dir = -1.0f;
                if (wrld.slice_w() < -12.0f) auto_dir =  1.0f;
                w_axis = auto_dir;
                // Rotate as well as translate, because rotation is the
                // half that looks four-dimensional: a sweep back and
                // forth through most of a quarter turn shows the
                // cross-sections changing without anyone touching the
                // wheel.
                //
                // Not a SLOW sweep, whatever an earlier comment here
                // said. 0.06 rad/s is twenty scroll notches a second and
                // moves roughly four fifths of the visible columns in
                // that second. It is a demonstration rate, chosen so the
                // change is unmistakable to someone watching a recording,
                // and it is well above anything a player produces by
                // hand.
                static float auto_theta_dir = 1.0f;
                if (wrld.slice_theta() >  1.5f) auto_theta_dir = -1.0f;
                if (wrld.slice_theta() < -1.5f) auto_theta_dir =  1.0f;
                wrld.rotate_slice(auto_theta_dir * 0.06f * static_cast<float>(dt),
                                  cam.position().z);
            }
            // . and , kept as aliases so anything that documented them
            // still works, but E and Q are the bindings that matter.
            // Scroll rotates the cut. This is the control that makes the
            // world four-dimensional in the way a player can see standing
            // still: a tilted hyperplane meets the 4D lattice at an angle,
            // so terrain presents a different cross-section and structures
            // appear to change shape. Travelling along w only ever swaps
            // one axis-aligned world for another.
            // Not while the pointer is over the HUD. ImGui's GLFW
            // backend chains the previously installed scroll callback
            // rather than replacing it, so without this a wheel event
            // over a panel scrolls the panel AND rotates the world.
            const float scroll = hud.wants_mouse() ? 0.0f : input.scroll_dy();
            if (scroll != 0.0f) {
                // 0.003 rad a notch, about a fifth of a degree.
                //
                // Fine, but not as fine as it sounds: a rotated
                // hyperplane diverges from the original in proportion to
                // distance, so the far edge of the window moves several
                // times more than the ground under the player. Measured
                // against a flat slice over a 512-block window:
                //
                //     0.003 rad  19% of columns change, max jump  2
                //     0.010      50%                         6
                //     0.030      73%                        15
                //     0.050      81%                        17
                //
                // The first value chosen was 0.05, which moves four
                // fifths of the visible columns in one notch - the scene
                // being replaced rather than reshaped.
                //
                // Clamped per frame, because a wheel is not a keyboard.
                // GLFW reports kinetic trackpad scrolling as a stream of
                // large deltas, and the accumulator adds them all up
                // between frames, so one flick could apply an arbitrary
                // angle in a single step - past a quarter turn, which no
                // amount of streaming budget can follow and which reads
                // as the world being swapped. Eight notches is a fast
                // deliberate turn and already moves about half the
                // window; anything above it is the input device, not the
                // player.
                constexpr float kMaxNotchesPerFrame = 8.0f;
                const float notches =
                    std::clamp(scroll, -kMaxNotchesPerFrame, kMaxNotchesPerFrame);
                wrld.rotate_slice(notches * 0.003f, cam.position().z);
            }

            if (!opt.auto_w) {
            if (input.key_down(core::key_of(core::Bind::SliceForward)) ||
                input.key_down(GLFW_KEY_PERIOD)) w_axis += 1.0f;
            if (input.key_down(core::key_of(core::Bind::SliceBack)) ||
                input.key_down(GLFW_KEY_COMMA))  w_axis -= 1.0f;
            }
            if (w_axis != 0.0f) {
                // Sprint applies here too, so the fourth axis handles like
                // the other three.
                const float w_speed = input.key_down(GLFW_KEY_LEFT_SHIFT)
                    ? world::World::kSprintSpeedW : world::World::kWalkSpeedW;
                // Position only. The rebuild is not driven from here any
                // more - see stream_slice below, which runs every frame
                // whether or not the player is moving, so chunks left
                // behind by a budget-limited frame still catch up after
                // the key is released.
                wrld.advance_w(w_axis * w_speed * static_cast<float>(dt));
            }
        }
        if (input.key_pressed(core::key_of(core::Bind::Vsync))) {
            vsync_enabled = !vsync_enabled;
            win->set_vsync(vsync_enabled);
            std::printf("[gfx] vsync %s\n", vsync_enabled ? "on" : "off");
        }
        if (input.key_pressed(core::key_of(core::Bind::Wireframe))) {
            wireframe = !wireframe;
            std::printf("[gfx] wireframe %s\n", wireframe ? "on" : "off");
        }
        if (input.key_pressed(core::key_of(core::Bind::Save))) {
            save_world_to_disk(wrld, terrain_seed);
        }
        if (input.key_pressed(core::key_of(core::Bind::Load))) {
            load_world_from_disk(wrld, pool, terrain_seed);
            // Reset streaming bookkeeping so the next move triggers a refill
            // around the player for anything missing on disk.
            last_center = world::ChunkCoord{
                static_cast<std::int32_t>(std::floor(cam.position().x / world::kChunkSizeX)) + 1,
                last_center.z};
        }
        if (input.key_down(core::key_of(core::Bind::StepTimeForward))) time_of_day += dt * 0.05f;
        if (input.key_down(core::key_of(core::Bind::StepTimeBack)))  time_of_day -= dt * 0.05f;
        if (!time_paused) time_of_day += dt * day_speed;
        time_of_day -= std::floor(time_of_day);

        bool copy_perf_requested = input.key_pressed(core::key_of(core::Bind::CopyPerf));
        if (input.key_pressed(core::key_of(core::Bind::WalkFly))) {
            walk_mode = !walk_mode;
            if (walk_mode) {
                player.set_position(cam.position()
                                    - glm::vec3(0.0f, game::Player::kEyeHeight, 0.0f));
            }
            std::printf("[mode] %s\n", walk_mode ? "walk" : "fly");
        }

        for (int k = 0; k < world::kMaxBlockId; ++k) {
            if (input.key_pressed(GLFW_KEY_1 + k)) {
                place_id = static_cast<world::BlockId>(k + 1);
                std::printf("[place] %s\n", block_name(place_id));
            }
        }
        // Scripted capture locks the pose: live mouse/keys would steer the
        // camera mid-run and make the shot non-reproducible.
        // Pull the world toward the player's w, a bounded slice of it per
        // frame. Runs every frame rather than on a key, so the terrain
        // keeps converging after the player stops travelling.
        if (wrld.is_4d()) wrld.stream_slice(terrain, pool);

        capture.shot_after = shot_after;  // counts down as the shot settles
        // Held M, or the middle mouse button, turns the 4D cut with the
        // mouse instead of turning the camera - vertical in the ZW plane,
        // horizontal in XW. The wheel remains a ZW-only shortcut.
        const bool slice_look =
            wrld.is_4d() && !capture.scripted_camera() &&
            (input.key_down(core::key_of(core::Bind::SliceLook)) ||
             input.mouse_button_down(GLFW_MOUSE_BUTTON_MIDDLE));
        if (input.cursor_captured() && !capture.scripted_camera()) {
            update_movement(input, dt, cam, player, wrld, walk_mode, slice_look);
            // Block interaction is suppressed while turning the cut: a
            // middle-drag that also placed a block would be a trap.
            if (!slice_look) {
                handle_block_interaction(input, cam, player, walk_mode, wrld,
                                         place_id);
            }
        }
        if (slice_look && input.cursor_captured()) {
            // Same per-notch scale as the wheel, so a pixel of mouse and a
            // notch of wheel move the world by comparable amounts.
            constexpr float kSliceLookScale = 0.0016f;
            const float dz = input.mouse_dy() * kSliceLookScale;
            const float dx = input.mouse_dx() * kSliceLookScale;
            if (dz != 0.0f) wrld.rotate_slice(dz, cam.position().z);
            if (dx != 0.0f) wrld.rotate_slice_xw(dx, cam.position().x);
        }

        // Scripted captures drive the camera themselves. Both step by frame
        // index, not dt, so a slow frame cannot put a hitch into the
        // assembled clip. The orbit flies a fixed-step circle at constant
        // height, always looking at the scene center; the cycle parks at
        // the orbit's start pose and spends the frames on one full day of
        // time-of-day instead.
        // The tilt clip holds the camera and turns the 4D cut instead -
        // the one capture where the world moves and the viewer does not.
        //
        // A ping-pong through sin, not a ramp: the sweep has to return to
        // where it started or the GIF's last frame will not meet its
        // first, and a rotation has no period short enough to loop on.
        if (tilt_frames > 0 && world_settled) {
            // --pose-at wins if given; otherwise park at the orbit's
            // start, which looks across the spawn triple point.
            if (!have_pose_at) {
                const OrbitPose op = orbit_pose_at(0, 1, orbit_center);
                cam.set_position(op.pos);
                cam.set_yaw_pitch(op.yaw, op.pitch);
            }
            // Bigger than the stills' 0.08 on purpose. The pivot is the
            // viewer, so the ground under the camera barely moves however
            // far the cut turns - the change lives in the middle distance,
            // and a sweep has to be wide enough to carry it there.
            const float kTiltAmplitude =
                (opt.slice_tilt != 0.0f) ? std::fabs(opt.slice_tilt) : 0.15f;
            const float phase = 6.28318530718f *
                static_cast<float>(capture_frame) /
                static_cast<float>(tilt_frames);
            const float want = kTiltAmplitude * std::sin(phase);
            wrld.rotate_slice(want - wrld.slice_theta(), cam.position().z);
        }
        if ((orbit_frames > 0 || cycle_frames > 0) && world_settled) {
            // Cycle parks at the orbit start (frame 0) and spends its
            // frames on time-of-day; orbit sweeps the full circle.
            const OrbitPose op = (orbit_frames > 0)
                ? orbit_pose_at(capture_frame, orbit_frames, orbit_center)
                : orbit_pose_at(0, 1, orbit_center);
            cam.set_position(op.pos);
            cam.set_yaw_pitch(op.yaw, op.pitch);
            if (cycle_frames > 0) {
                time_of_day = std::fmod(
                    0.35f + static_cast<float>(capture_frame) /
                                static_cast<float>(cycle_frames),
                    1.0f);
            }
        }

        // Orbit frame benchmark: sweep one revolution across the sampled
        // frames. The sample count is the phase, so during the settle
        // period (no samples yet) the camera holds at the start pose, then
        // moves one step per sampled frame. Camera motion drives chunk
        // streaming, so this bench includes the upload cost a static pose
        // never pays.
        if (bench_frames > 0 && bench_orbit && world_settled) {
            const OrbitPose op = orbit_pose_at(
                sampler.collected(), bench_frames,
                orbit_center);
            cam.set_position(op.pos);
            cam.set_yaw_pitch(op.yaw, op.pitch);
        }

        world::ChunkCoord center{
            static_cast<std::int32_t>(std::floor(cam.position().x / world::kChunkSizeX)),
            static_cast<std::int32_t>(std::floor(cam.position().z / world::kChunkSizeZ))
        };
        if (initial_load_logged && !(center == last_center)) {
            auto sstats = wrld.update_streaming(center, stream_radius, terrain, pool);
            streamed_in_total  += sstats.requested;
            streamed_out_total += sstats.evicted;
            last_center = center;
        }
        // Drain harder while travelling along w.
        //
        // Uploading is the bottleneck when the whole window is being
        // pulled toward a new w, not generating: nine workers produce
        // chunks far faster than 16 a frame can be handed to the GPU, so
        // the queue backs up and the far edge of the world visibly trails.
        // Uploads are ~0.05 ms each, so 48 is well under a millisecond of
        // frame time and only happens while the player is actually
        // moving through the fourth dimension.
        wrld.drain_finished(wrld.is_4d() && wrld.pending_async() > 32 ? 48 : 16);
        // Chunks meshed before their neighbours existed still carry the
        // boundary faces those neighbours hide. Driven here rather than
        // from update_streaming because it depends on chunks arriving, not
        // on the camera moving.
        // Held back until the initial load reports: re-mesh jobs share the
        // pool with terrain jobs, and letting them compete would slow the
        // load and change the chunks/sec figure that load measures. Once
        // it has reported, drain hard until the world settles, then
        // trickle so a chunk streaming in mid-play cannot spike a frame.
        if (initial_load_logged) {
            wrld.flush_pending_remeshes(pool, world_settled ? 4 : 64);
        }
        if (initial_load_logged && !world_settled &&
            wrld.pending_async() == 0 && wrld.pending_remesh() == 0) {
            world_settled = true;
            settle_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - async_t0).count()
                - initial_load_ms;
            // Reported rather than folded into the load figure: this is
            // what cross-chunk culling costs at startup, and hiding it
            // inside chunks/sec would misstate both numbers.
            std::printf("[world]   boundary re-mesh settle %.1f ms after "
                        "load (chunks meshed before their neighbours "
                        "existed)\n", settle_ms);
        }

        if (!initial_load_logged && wrld.pending_async() == 0) {
            initial_load_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - async_t0).count();
            initial_load_logged = true;
            double cps = initial_load_ms > 0.0
                ? total_chunks * 1000.0 / initial_load_ms : 0.0;
            std::printf("[world] %d chunks loaded in %.1f ms  (%.0f chunks/sec, %zu workers)\n",
                        total_chunks, initial_load_ms, cps, worker_count);
            const double w_total = wrld.total_worker_ms();
            const double t_total = wrld.total_terrain_ms();
            const double m_total = wrld.total_mesh_ms();
            const double u_total = wrld.total_upload_ms();
            std::printf("[world]   worker total %.1f ms (avg %.2f ms/chunk, %.1fx wall-clock across %zu workers)\n",
                        w_total, w_total / total_chunks,
                        w_total / std::max(initial_load_ms, 0.001),
                        worker_count);
            std::printf("[world]     terrain.fill_chunk total %.1f ms (avg %.2f ms/chunk)\n",
                        t_total, t_total / total_chunks);
            std::printf("[world]     greedy mesh        total %.1f ms (avg %.2f ms/chunk)\n",
                        m_total, m_total / total_chunks);
            std::printf("[world]   main-thread upload   total %.1f ms (avg %.2f ms/chunk on main thread)\n",
                        u_total, u_total / total_chunks);

            if (bench_io) {
                // Deterministic save/load throughput. Same loaded world the
                // cull bench measures; writes to /tmp to keep ./saves clean.
                namespace fsi = std::filesystem;
                const fsi::path io_dir = fsi::temp_directory_path() / "voxel-bench-io";
                fsi::remove_all(io_dir);
                fsi::create_directories(io_dir);

                // Order-independent checksum over every resident block, so a
                // save then load can be proven lossless, not just error-free.
                // Each chunk folds its coord and blocks with FNV-1a; the
                // per-chunk hashes are XORed, which does not depend on the
                // unordered map's iteration order.
                auto world_checksum = [&]() {
                    std::uint64_t combined = 0;
                    wrld.for_each_chunk(
                        [&](world::ChunkCoord c, const world::Chunk& ch) {
                            std::uint64_t h = 1469598103934665603ull;
                            const auto mix = [&h](std::uint64_t v) {
                                h = (h ^ v) * 1099511628211ull;
                            };
                            mix(static_cast<std::uint32_t>(c.x));
                            mix(static_cast<std::uint32_t>(c.z));
                            for (int y = 0; y < world::kChunkSizeY; ++y)
                                for (int z = 0; z < world::kChunkSizeZ; ++z)
                                    for (int x = 0; x < world::kChunkSizeX; ++x)
                                        mix(static_cast<std::uint64_t>(
                                            ch.get(x, y, z)));
                            combined ^= h;
                        });
                    return combined;
                };
                const std::uint64_t checksum_before = world_checksum();

                using clock = std::chrono::steady_clock;
                const auto save_t0 = clock::now();
                auto s = world::save_world(wrld, io_dir.string(), terrain_seed);
                const double save_ms = std::chrono::duration<double, std::milli>(
                    clock::now() - save_t0).count();

                wrld.clear_all();

                const auto load_t0 = clock::now();
                auto l = world::load_world(wrld, io_dir.string(), pool, terrain_seed);
                const double load_ms = std::chrono::duration<double, std::milli>(
                    clock::now() - load_t0).count();

                const std::uint64_t checksum_after = world_checksum();
                const bool roundtrip_ok = checksum_before == checksum_after;

                fsi::remove_all(io_dir);

                const double save_secs = save_ms / 1000.0;
                const double load_secs = load_ms / 1000.0;
                const double save_disk_mbps = save_secs > 0.0
                    ? (s.bytes_written / (1024.0 * 1024.0)) / save_secs : 0.0;
                const double save_raw_mbps  = save_secs > 0.0
                    ? (s.bytes_raw     / (1024.0 * 1024.0)) / save_secs : 0.0;
                const double load_disk_mbps = load_secs > 0.0
                    ? (l.bytes_read    / (1024.0 * 1024.0)) / load_secs : 0.0;
                const double load_raw_mbps  = load_secs > 0.0
                    ? (l.bytes_raw     / (1024.0 * 1024.0)) / load_secs : 0.0;
                const double ratio = s.bytes_written > 0
                    ? static_cast<double>(s.bytes_raw) / s.bytes_written : 0.0;

                std::printf("\nBENCH_IO radius=%d chunks=%d"
                            " save_ms=%.1f load_ms=%.1f"
                            " disk_mb=%.2f raw_mb=%.2f ratio=%.1fx"
                            " save_disk_mbps=%.0f save_raw_mbps=%.0f"
                            " load_disk_mbps=%.0f load_raw_mbps=%.0f"
                            " save_ok=%d load_ok=%d roundtrip_ok=%d\n",
                            stream_radius, s.chunks_written,
                            save_ms, load_ms,
                            s.bytes_written / (1024.0 * 1024.0),
                            s.bytes_raw     / (1024.0 * 1024.0),
                            ratio,
                            save_disk_mbps, save_raw_mbps,
                            load_disk_mbps, load_raw_mbps,
                            s.ok ? 1 : 0, l.ok ? 1 : 0, roundtrip_ok ? 1 : 0);
                std::fflush(stdout);
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w != postfx_w || fb_h != postfx_h) {
            // init() destroys the old targets first, so a failure here
            // leaves the chain unusable and every later frame renders
            // through a 0x0 framebuffer. Say so once instead of emitting
            // an incomplete-FBO warning per frame forever.
            if (!postfx.init(fb_w, fb_h) && !postfx_failed) {
                postfx_failed = true;
                std::fprintf(stderr,
                             "[postfx] re-init failed at %dx%d; "
                             "post-processing is disabled for this run\n",
                             fb_w, fb_h);
            }
            postfx_w = fb_w;
            postfx_h = fb_h;
        }
        float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;

        render::FrameView fv;
        fv.view       = cam.view_matrix();
        fv.camera_pos = cam.position();
        fv.window_w   = fb_w;
        fv.window_h   = fb_h;
        fv.fog_end    = static_cast<float>(stream_radius * world::kChunkSizeX) * 0.95f;
        fv.fog_start  = fv.fog_end * 0.85f;  // keep midrange crisp; haze only far out
        // Camera far plane sits just past the fog plane: anything further is
        // fully fogged out and contributes nothing. Tightening it from the
        // 500 m default also gives the frustum a real far-plane cull instead
        // of one that never trips at radius 12.
        const float kCameraFar = fv.fog_end + static_cast<float>(world::kChunkSizeX);
        fv.proj       = cam.proj_matrix(aspect, 70.0f, 0.1f, kCameraFar);
        // Scripted captures freeze the water phase: shots stay diffable and
        // the orbit's last frame meets its first.
        fv.time_seconds = capture.scripted_camera()
                              ? 100.0f
                              : static_cast<float>(now);

        render::LightingFrame light = render::compute_lighting(time_of_day);

        // Stagger: refresh cascade c only every (1 << c) frames. The far
        // cascade is hundreds of meters wide and barely changes frame to
        // frame, so paying 3x shadow cost to refresh near-stale data is
        // wasted work. c1 and c2 are phased so they never coincide with
        // each other - peak passes/frame stays at 2 instead of 3, keeping
        // the frame-time envelope flat:
        //   c0 every frame  c1 on (f & 1) == 0  c2 on (f & 3) == 1
        // Avg = 1 + 0.5 + 0.25 = 1.75 passes/frame.
        uint32_t shadow_cascade_mask = 1u;  // c0 always
        if ((frame_index & 1ull) == 0ull)        shadow_cascade_mask |= (1u << 1);
        if ((frame_index & 3ull) == 1ull)        shadow_cascade_mask |= (1u << 2);
        // First frame: refresh everything so caches are valid.
        if (frame_index == 0ull) shadow_cascade_mask = (1u << gfx::kNumCascades) - 1u;
        // When shadows just transitioned 0 -> active (sunrise), the cached
        // depth textures and matrices are stale from before the night
        // skip-pass - force-refresh all cascades to resync.
        const bool shadow_active_now = (light.shadow_strength > 0.0f);
        if (shadow_active_now && !prev_shadow_active) {
            shadow_cascade_mask = (1u << gfx::kNumCascades) - 1u;
        }
        prev_shadow_active = shadow_active_now;
        auto cascades = gfx::CascadedShadowMap::fit_cascades(
            fv.view, fv.proj, light.sun_dir, kShadowNear, kShadowFar,
            0.5f, kShadowMapSize);
        for (int c = 0; c < gfx::kNumCascades; ++c) {
            if (shadow_cascade_mask & (1u << c)) {
                cached_light_vp[c]    = cascades[c].light_vp;
                cached_cascade_far[c] = cascades[c].split_far_view;
            }
            fv.light_vp[c]    = cached_light_vp[c];
            fv.cascade_far[c] = cached_cascade_far[c];
        }

        gfx::Frustum view_frustum;
        view_frustum.from_view_proj(fv.proj * fv.view);

        // Shadow pass writes to its own FBO; the other scene passes write
        // into the HDR FBO via begin_scene().
        sampler.begin_pass();
        render::draw_shadow_pass(shadow_map, shaders.shadow, wrld, fv, light,
                                 shadow_cascade_mask);
        sampler.end_pass(sampler.passes().shadow);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, block_atlas);

        postfx.begin_scene();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // Sky last, on purpose: draw_sky depth-tests its far-plane triangle
        // against what the terrain just wrote, so the procedural clouds and
        // stars only shade the pixels the world left empty. --sky-overdraw
        // restores the old sky-first order for the A/B.
        if (sky_overdraw) {
            sampler.begin_pass();
            render::draw_sky(shaders.sky, sky_vao, fv, light, false);
            sampler.end_pass(sampler.passes().sky);
        }
        sampler.begin_pass();
        // Wireframe wraps only the terrain color pass; the shadow depth pass
        // is already done and the sky, water, and post-process fullscreen
        // quad must stay filled, so bracket the draw and restore immediately.
        if (wireframe) {
            // Dark constant-colour lines on the flat wireframe shader, not
            // the terrain shader in line mode. The terrain shader fogs its
            // output, so wireframe edges used to fade into the horizon at
            // exactly the distance you need to stand back to fit a chunk
            // in frame - which made the merged rectangles unphotographable.
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            last_stats = render::draw_terrain_wireframe(
                shaders.wireframe, wrld, fv, view_frustum,
                glm::vec3(0.06f, 0.07f, 0.09f));
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        } else {
            last_stats = render::draw_terrain(shaders.terrain, shadow_map, wrld,
                                              fv, light, kBlockPalette,
                                              view_frustum,
                                              occlusion_cull_enabled);
        }
        sampler.end_pass(sampler.passes().terrain);
        if (!sky_overdraw) {
            sampler.begin_pass();
            render::draw_sky(shaders.sky, sky_vao, fv, light, true);
            sampler.end_pass(sampler.passes().sky);
        }
        sampler.begin_pass();
        // --only-chunk suppresses the water plane too. It is a capture aid
        // for looking at one chunk's meshed geometry, and an ocean drawn
        // across the whole frame is the thing that made the old wireframe
        // shot unreadable in the first place. Water is not mesher output.
        if (!only_chunk) {
            render::draw_water(shaders.water, water, fv, light,
                               static_cast<float>(world::kSeaLevel));
        }
        sampler.end_pass(sampler.passes().water);

        // Same ray the place/break logic uses, so the outline matches a
        // potential click target.
        //
        // Suppressed for stills and clips: a reticle and a selection box
        // are interface, not scene, and every capture this repo commits is
        // meant to show the renderer. They were quietly appearing in the
        // middle of every documentation image.
        const bool capturing_image = capture.suppresses_interface();
        world::World::RayHit target{};
        if (!capturing_image) {
            target = wrld.raycast(cam.position(), cam.forward(), 8.0f);
        }
        if (!capturing_image) render::draw_crosshair_and_selection(
            shaders.wireframe, selection_cube,
            shaders.crosshair, crosshair_vao,
            fv,
            target.hit,
            target.block_x, target.block_y, target.block_z);

        // HDR -> bright extract -> blur -> ACES tonemap to backbuffer.
        sampler.begin_pass();
        postfx.resolve_to_backbuffer(shaders.bright, shaders.bloom_down,
                                     shaders.bloom_up, shaders.tonemap,
                                     fb_w, fb_h,
                                     /*threshold*/ 1.0f,
                                     /*intensity*/ 0.7f,
                                     /*exposure*/  1.0f);
        sampler.end_pass(sampler.passes().postfx);

        // Scripted clip capture: save the frame just rendered (pre-HUD),
        // one PNG per step after a settle period for streaming and shadows.
        const int capture_frames = capture.image_sequence_frames();
        if (capture_frames > 0 && world_settled) {
            constexpr int kCaptureSettleFrames = 90;
            if (capture_settle < kCaptureSettleFrames) {
                ++capture_settle;
            } else {
                // The tilt clip changes the WHOLE window between frames,
                // so it converges before the shot rather than riding the
                // streaming budget the way a moving camera can. A GIF of
                // a half-built world is worse than no GIF.
                //
                // Here, not next to the rotation at the top of the loop:
                // there it ran on all 90 settle frames as well, and a
                // convergence that fell back on its deadline burned
                // twenty seconds ninety times before the first PNG was
                // written. Six frames took over ten minutes.
                if (tilt_frames > 0) {
                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::seconds(10);
                    bool converged = false;
                    while (std::chrono::steady_clock::now() < deadline) {
                        wrld.drain_finished(256);
                        wrld.flush_pending_remeshes(pool, 256);
                        if (wrld.pending_async() == 0 &&
                            wrld.pending_remesh() == 0 &&
                            wrld.stream_slice(terrain, pool, 256) == 0) {
                            converged = true;
                            break;
                        }
                        std::this_thread::yield();
                    }
                    if (!converged) {
                        std::fprintf(stderr, "[capture] frame %d did not "
                                     "converge; %d chunks stale\n",
                                     capture_frame, wrld.slice_lag().stale);
                    }
                }
                char frame_name[32];
                std::snprintf(frame_name, sizeof(frame_name),
                              "frame_%04d.png", capture_frame);
                if (gfx::save_screenshot(fb_w, fb_h, "./capture",
                                         frame_name).empty()) {
                    std::fprintf(stderr, "[capture] write failed at %s\n",
                                 frame_name);
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                if (++capture_frame == capture_frames) {
                    std::printf("[capture] %d frames -> ./capture\n",
                                capture_frames);
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
        }

        // Headless --save: once the world has finished generating, write it
        // to disk and exit. The RLE snapshot is chunk data, so no settle
        // frames are needed -- the world is complete when streaming drained.
        if (!save_path.empty() && world_settled) {
            auto t0 = std::chrono::steady_clock::now();
            auto s = world::save_world(wrld, save_path, terrain_seed);
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            const double ratio = s.bytes_written > 0
                ? static_cast<double>(s.bytes_raw) / s.bytes_written : 0.0;
            std::printf("[save] wrote %d chunks to %s in %.1f ms  |  "
                        "%.2f MB on disk, %.1fx ratio  |  %s\n",
                        s.chunks_written, save_path.c_str(), ms,
                        s.bytes_written / (1024.0 * 1024.0), ratio,
                        s.ok ? "ok" : "ERRORS");
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Emissive blocks for a capture. Placed once the world has
        // settled, in a fixed ring around the camera so the shot is
        // deterministic, and only into air so nothing is destroyed.
        if (demo_lights_pending && world_settled) {
            demo_lights_pending = false;
            const glm::vec3 p = cam.position();
            int placed = 0;
            for (int i = 0; i < 16; ++i) {
                const float a = 6.2831853f * static_cast<float>(i) / 16.0f;
                for (int r = 4; r <= 12; r += 4) {
                    const int bx = static_cast<int>(std::floor(p.x + std::cos(a) * r));
                    const int bz = static_cast<int>(std::floor(p.z + std::sin(a) * r));
                    for (int dy = -3; dy <= 3; ++dy) {
                        const int by = static_cast<int>(std::floor(p.y)) + dy;
                        if (by < 1 || by >= world::kChunkSizeY) continue;
                        if (wrld.block_at(bx, by, bz) != world::BlockId::Air) continue;
                        if (wrld.set_block(bx, by, bz, world::BlockId::Glow)) ++placed;
                        break;
                    }
                }
            }
            std::printf("[demo] placed %d light sources around the camera\n", placed);
        }

        // Headless --validate: once the world is resident, read every mesh
        // back off the GPU, check each triangle against the voxel data, and
        // exit nonzero on offenders. Composes with --load to verify a saved
        // world and with --seed to spot-check other maps.
        //
        // Gated on world_settled, not just on residency: chunks meshed
        // before their neighbours arrived are still owed a re-mesh, and
        // validating mid-convergence reports the pre-culling footprint.
        if (validate_mode && world_settled) {
            const int bad = wrld.debug_validate_gpu_meshes();
            // The engine's own resident mesh footprint, printed here
            // because this is the only headless mode that builds a real
            // world on a real GPU. --bench computes the same figure from
            // the mesher alone; the two agreeing is what says the
            // streaming path is uploading what the mesher produces.
            // 11,528,256 bytes on the default seed and radius, and it
            // reproduces exactly - better than twenty consecutive runs,
            // including immediately after a full audit.
            //
            // It printed 11.00 twice during one session and never
            // reproduced in isolation. Both times another process was
            // building into the same build/ directory, so the likeliest
            // explanation is that those runs used a binary that was being
            // relinked underneath them rather than that the figure moves.
            // Recorded rather than asserted, because the difference
            // between "deterministic" and "deterministic except twice" is
            // exactly the kind of thing this repo does not round off.
            //
            // The figure the CI gate actually bounds is world_mesh_mb from
            // --bench, which check_invariance proves byte-identical across
            // runs; this one corroborates it from the running engine.
            const double gpu_mb = static_cast<double>(wrld.resident_gpu_bytes())
                                  / (1024.0 * 1024.0);
            std::printf("\nVALIDATE chunks=%zu bad_triangles=%d "
                        "gpu_mesh_mb=%.2f %s\n",
                        wrld.chunk_count(), bad, gpu_mb,
                        bad == 0 ? "ok" : "FAILED");
            if (bad > 0) {
                return EXIT_FAILURE;
            }
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Headless --verify-4d: prove the fourth axis is real and
        // reversible.
        //
        // Three claims, in order. A step along w must CHANGE the world -
        // otherwise the axis exists in the storage and does nothing.
        // Stepping back must return the world to exactly what it was,
        // which is the determinism the whole design rests on: slices are
        // a pure function of (seed, w), so w=0 reached by stepping is
        // w=0 reached by starting there. And the meshes have to be right
        // at each stop, checked by the same GPU read-back --validate
        // uses, because a slice change re-meshes every chunk in the
        // window and that is the most likely thing to go wrong.
        if (verify_4d && world_settled) {
            auto settle = [&]() {
                // Drain until every re-requested chunk has landed and the
                // boundary re-meshes owed to it are flushed.
                //
                // Bounded by TIME, not by iteration count. The first
                // version guarded with `for (guard < 100000)`, which is a
                // spin count: the loop body is a few microseconds when
                // there is nothing to drain, so it burned through all
                // 100,000 iterations in milliseconds and returned while
                // workers were still busy. The hash was then taken on a
                // half-rebuilt world, and whether the check passed
                // depended on how much unrelated work happened to run
                // first - adding a debug print "fixed" it.
                //
                // Yielding matters as much as the deadline: the drain is
                // main-thread work waiting on nine workers, and spinning
                // without yielding steals the core they need.
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(60);
                while (std::chrono::steady_clock::now() < deadline) {
                    wrld.drain_finished(256);
                    wrld.flush_pending_remeshes(pool, 256);
                    if (wrld.pending_async() == 0 &&
                        wrld.pending_remesh() == 0) break;
                    std::this_thread::yield();
                }
            };
            auto world_hash = [&wrld]() {
                std::uint64_t h = 1469598103934665603ull;
                std::vector<world::ChunkCoord> coords;
                wrld.for_each_chunk([&](world::ChunkCoord c, const world::Chunk&) {
                    coords.push_back(c);
                });
                std::sort(coords.begin(), coords.end(),
                          [](const world::ChunkCoord& a, const world::ChunkCoord& b) {
                              return a.z != b.z ? a.z < b.z : a.x < b.x;
                          });
                for (const auto& c : coords)
                    for (int y = 0; y < world::kChunkSizeY; ++y)
                        for (int z = 0; z < world::kChunkSizeZ; ++z)
                            for (int x = 0; x < world::kChunkSizeX; ++x) {
                                const auto b = static_cast<std::uint8_t>(
                                    wrld.block_at(c.x * world::kChunkSizeX + x, y,
                                                  c.z * world::kChunkSizeZ + z));
                                h = (h ^ b) * 1099511628211ull;
                            }
                return h;
            };

            // Travels to a target w and drives the rebuild until the
            // geometry has caught up.
            //
            // move_w declines a rebuild while the previous one is still
            // draining - that throttle is what stops a held key from
            // saturating the pool with work it will discard - so a check
            // that assumed one call rebuilds would be testing an engine
            // that does not exist. This drives it the way the render loop
            // does, without waiting for frames.
            auto travel_to = [&](float target) {
                wrld.move_w(target - wrld.slice_w(), 0.0f, terrain, pool);
                for (int guard = 0; guard < 1000; ++guard) {
                    settle();
                    if (std::fabs(wrld.meshed_w() - wrld.slice_w()) < 1e-4f) break;
                    wrld.resample_slice(terrain, pool);
                }
            };

            const float w0 = wrld.slice_w();
            const std::uint64_t hash_w0 = world_hash();
            const int bad_w0 = wrld.debug_validate_gpu_meshes();

            const auto step_t0 = std::chrono::steady_clock::now();
            const int requested = wrld.move_w(+1.0f, 0.0f, terrain, pool);
            travel_to(w0 + 1.0f);
            const double step_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - step_t0).count();
            const std::uint64_t hash_w1 = world_hash();
            const int bad_w1 = wrld.debug_validate_gpu_meshes();

            travel_to(w0);
            const std::uint64_t hash_back = world_hash();
            const int bad_back = wrld.debug_validate_gpu_meshes();

            // There is no rapid-stepping phase, and its absence is
            // deliberate rather than an omission.
            //
            // One used to sit here, asserting that several rebuilds in
            // flight at once still leave the world byte-identical. Three
            // measurements retired it:
            //
            //   - the scenario is unreachable through move_w. resample_slice
            //     declines while any job is in flight, so 1 of 6 rapid
            //     calls issued a rebuild and 5 were refused by the
            //     throttle; there is only ever one rebuild in flight.
            //   - the stale-result stamp it leaned on fired zero times
            //     across a whole run. It IS reachable - oscillating the
            //     stream window while under-draining hits it 195 times -
            //     but every phase here settles first, so this mode never
            //     gets near it.
            //   - and even when reached, the stamp is not a content
            //     invariant. With the guard removed, 287-302 stale
            //     results were accepted and the settled world was still
            //     byte-identical: drain_finished stamps each slot from
            //     its own job's slice and stream_slice re-requests
            //     anything that does not match, so it self-heals.
            //
            // So no world-hash check can fail when that guard is removed,
            // and a phase asserting otherwise asserts something false.
            // The stamp is an efficiency mechanism - do not apply work
            // you would only redo - not a correctness one.

            // Captured before the held-key phase below, which deliberately
            // leaves the player somewhere else along w. Asserting it after
            // that phase compared the post-travel position against the
            // start and failed every run - the check was reporting a
            // failure of its own bookkeeping while every real property
            // passed.
            const bool position_ok = std::fabs(wrld.slice_w() - w0) < 1e-4f;

            // Rotating the slice, which is the motion that makes this look
            // four-dimensional rather than merely indexed by a fourth
            // number. Same three claims as travelling - it must change the
            // world, it must be reversible, and the meshes must be right
            // at the tilted stop - but along the axis that produces cross
            // sections instead of swapping one axis-aligned world for
            // another.
            //
            // This exists because the rotation shipped with generator-level
            // tests and nothing at the engine level. The generator checks
            // prove a tilted slice samples a different heightfield; they
            // say nothing about whether World notices theta changed, whose
            // per-chunk staleness is a separate mechanism with its own
            // 32-block lever arm, and which was the part more likely to
            // silently do nothing.
            //
            // Driven through stream_slice, which is the path the scroll
            // wheel actually reaches, and NOT through resample_slice.
            //
            // The first version of this phase called resample_slice, and
            // it passed with the per-chunk tilt staleness deleted -
            // because resample_slice rebuilds the whole window
            // unconditionally, so the world changed no matter what the
            // staleness test said. It was checking that a tilted slice
            // generates different terrain, which the generator tests
            // already prove, rather than that World NOTICES a tilt. The
            // fault injection is the only reason that surfaced.
            //
            // Rotation has no analogue of meshed_w to converge on, so the
            // convergence test is the world's own: keep streaming until a
            // settled world stops asking for chunks.
            //
            // An iteration guard is correct HERE, unlike in --bench-4d,
            // and the difference is whether the body can no-op. Each pass
            // calls settle() first, which is deadline-bounded and does not
            // return until the pool has drained, so an iteration is a
            // completed drain cycle rather than a spin. A guard around a
            // body that can return in two microseconds is a spin count
            // wearing a timeout's clothes.
            // Records a failure to converge rather than spinning on it.
            //
            // A world that never converges used to make this loop run its
            // full guard and return quietly, so the phases after it
            // measured a half-built world - or, with the guard large
            // enough, the run simply produced no VERIFY4D line at all and
            // CI saw an opaque timeout. A check that hangs instead of
            // failing tells you less than no check.
            bool settle_timed_out = false;
            auto settle_slice = [&]() {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(30);
                for (int guard = 0; guard < 4000; ++guard) {
                    settle();
                    if (wrld.stream_slice(terrain, pool) == 0) {
                        settle();
                        // The inner settle() has its own deadline and
                        // returns quietly when it blows it - and a world
                        // that is merely draining SLOWLY has every
                        // outstanding chunk in requested_, which makes
                        // stream_slice return 0. So the early return here
                        // could report a settled world that never
                        // drained. Checking the queues is what separates
                        // "nothing left to ask for" from "nothing left to
                        // do".
                        if (wrld.pending_async() != 0 ||
                            wrld.pending_remesh() != 0) {
                            break;
                        }
                        return;
                    }
                    if (std::chrono::steady_clock::now() > deadline) break;
                }
                settle_timed_out = true;
                std::fprintf(stderr, "[verify-4d] the world did not settle: "
                             "%d of %d chunks stale, %d jobs in flight, "
                             "%d re-meshes pending\n",
                             wrld.slice_lag().stale, wrld.slice_lag().resident,
                             wrld.pending_async(), wrld.pending_remesh());
            };
            // 0.25 rad, about 14 degrees. Far past the 0.003 a scroll notch
            // gives, so this is many notches of turning, and far short of
            // the pi/2 where the slice's z axis becomes w outright.
            constexpr float kTilt = 0.25f;
            const float theta0 = wrld.slice_theta();
            wrld.rotate_slice(+kTilt, 0.0f);
            settle_slice();
            const std::uint64_t hash_tilt = world_hash();
            const int bad_tilt = wrld.debug_validate_gpu_meshes();
            wrld.rotate_slice(-kTilt, 0.0f);
            settle_slice();
            const std::uint64_t hash_untilt = world_hash();
            const int bad_untilt = wrld.debug_validate_gpu_meshes();

            // One scroll notch, which is a much sharper check than the
            // large tilt above and pins a different thing.
            //
            // 0.25 rad is far over every threshold, so it passes with the
            // staleness lever arm removed - verified by injecting exactly
            // that: 32.0f -> 1.0f still reported tilt_changed=1. A single
            // notch is the case the lever arm exists for. 0.003 rad on its
            // own is below kSliceStepMin and no chunk would ever be
            // rebuilt; multiplied by the nominal 32-block arm it is 0.096
            // and the world responds to the very first notch. That is the
            // difference between a scroll wheel that works and one that
            // appears dead until you spin it far enough.
            constexpr float kNotch = 0.003f;   // main's scroll scale
            wrld.rotate_slice(+kNotch, 0.0f);
            settle_slice();
            const bool notch_moves_world = world_hash() != hash_w0;
            wrld.rotate_slice(-kNotch, 0.0f);
            settle_slice();

            // An edit must survive the SCROLL WHEEL, and this is a
            // separate check from edit_survives_w rather than a variation
            // on it, because it exercises a different code path.
            //
            // edit_survives_w drives travel_to -> move_w ->
            // resample_slice, which stashes every edited chunk before
            // rebuilding. The wheel drives stream_slice, which did not.
            // So the engine had a check that said edits survive travel
            // through the fourth dimension, passing, while one scroll
            // notch deleted every edit in the world - drift after one
            // notch is 0.003 * 32 = 0.096, six times kSliceStepMin, for
            // every chunk at once. Found by adversarial review, not by
            // this check, because this check did not exist.
            wrld.rotate_slice(0.0f, 0.0f);
            settle_slice();
            int sx = 0, sy = 0, sz = 0;
            bool scroll_placed = false;
            for (int i = 0; i < 256 && !scroll_placed; ++i) {
                sx = (i * 5) % 32;
                sy = 78 + (i % 6);
                sz = (i * 13) % 32;
                scroll_placed = wrld.set_block(sx, sy, sz, world::BlockId::Glow);
            }
            settle_slice();
            const bool scroll_edit_here =
                wrld.block_at(sx, sy, sz) == world::BlockId::Glow;
            // One notch, the smallest thing the wheel can do.
            wrld.rotate_slice(kNotch, 0.0f);
            settle_slice();
            const bool survives_notch =
                wrld.block_at(sx, sy, sz) == world::BlockId::Glow;
            // And a long scroll, which crosses no integer slice boundary
            // and so must not lose it either.
            for (int i = 0; i < 40; ++i) wrld.rotate_slice(kNotch, 0.0f);
            settle_slice();
            const bool survives_scroll =
                wrld.block_at(sx, sy, sz) == world::BlockId::Glow;
            wrld.rotate_slice(-kNotch * 41.0f, 0.0f);
            settle_slice();
            const bool edit_survives_scroll = scroll_placed && scroll_edit_here
                                              && survives_notch
                                              && survives_scroll;
            // Leave no edit behind for the phases that follow.
            wrld.set_block(sx, sy, sz, world::BlockId::Air);
            settle_slice();

            // The edit namespace must not move when the player does not.
            //
            // Edits are filed under an integer slice, and that used to be
            // floor(slice_w_) - a float that rotation rewrites every
            // notch. A round trip returns it to a residue rather than to
            // zero, and floor is one-sided there, so a value of -1e-16
            // read as slice -1 while every resident chunk still recorded
            // slice 0. The lookup missed, the terrain regenerated without
            // the edit, and the edit was filed under a key the player
            // would never consult again.
            //
            // Seven notches out and back, fifty blocks from spawn - which
            // is somebody trying the wheel out - was enough to trigger it.
            // That is what this reproduces.
            bool namespace_stable = true;
            {
                const std::int32_t before = wrld.edit_slice();
                for (const float pz : {50.0f, 137.0f, 640.0f}) {
                    for (const int n : {7, 40}) {
                        for (int i = 0; i < n; ++i) wrld.rotate_slice(kNotch, pz);
                        for (int i = 0; i < n; ++i) wrld.rotate_slice(-kNotch, pz);
                        if (wrld.edit_slice() != before) namespace_stable = false;
                    }
                }
                settle_slice();
            }

            // An edited chunk must keep rotating with the world.
            //
            // The edit stash used to hold the whole chunk and hand it
            // back verbatim, which preserved the edit and froze
            // everything around it: one placed block pinned its entire
            // 16x256x16 chunk against every further turn of the wheel.
            // Measured against an unedited control chunk at the same
            // distance from the pivot, sixty notches:
            //
            //     before   edited 40 -> 40 (frozen)   control 52 -> 42
            //     after    edited 40 -> 44            control 52 -> 42
            //
            // The control matters. A first version of this check compared
            // a chunk at slice-z 7 against one at slice-z 45 and read the
            // difference as a freeze - but a rotation displaces in
            // proportion to distance from the pivot, so that gap was the
            // pivot working correctly. Comparing at equal distance is
            // what makes the result mean anything.
            bool edit_rotates = false;
            {
                auto surface_of = [&](int wx, int wz) {
                    for (int y = world::kChunkSizeY - 1; y > 0; --y)
                        if (wrld.block_at(wx, y, wz) != world::BlockId::Air)
                            return y;
                    return 0;
                };
                // A whole chunk footprint, not one column.
                //
                // The first version compared a single column's integer
                // surface height and required it to differ. Whether any
                // ONE column's height changes is luck: the same check
                // passed at radius 4 and 8 and failed at 6, because at 6
                // the column it happened to pick landed on the same
                // integer either side of the rotation. Counting how much
                // of the chunk moved is the measurement that was meant.
                auto profile = [&](int ox, int oz, std::vector<int>& out) {
                    out.clear();
                    for (int z = 0; z < world::kChunkSizeZ; ++z)
                        for (int x = 0; x < world::kChunkSizeX; ++x)
                            out.push_back(surface_of(ox + x, oz + z));
                };
                auto differing = [](const std::vector<int>& a,
                                    const std::vector<int>& b) {
                    int n = 0;
                    for (std::size_t i = 0; i < a.size(); ++i)
                        if (a[i] != b[i]) ++n;
                    return n;
                };
                // Half the window, so this is well away from the pivot
                // at any radius and inside the window at any radius.
                //
                // It was a fixed z = 80, which is outside a radius-4
                // window: set_block returned false, put stayed false, and
                // the check failed on CI while passing locally at radius
                // 6. A check whose subject may not exist is worse than no
                // check - it reports a failure of its own arithmetic.
                const int half = (opt.stream_radius * world::kChunkSizeZ) / 2;
                // Chunk-ALIGNED footprints, or the measurement straddles
                // two chunks and only one of them is the subject.
                //
                // The first version profiled a 16x16 block starting at
                // the edited column, which spanned two chunks in x: with
                // the freeze injected, five columns of every row still
                // belonged to the unedited neighbour and moved, which was
                // enough to clear the threshold and pass. The check
                // reported the feature working while the defect it exists
                // for was present.
                const int ez = (half / world::kChunkSizeZ) * world::kChunkSizeZ;
                const int edited_x = 0;
                const int control_x = edited_x + 2 * world::kChunkSizeX;
                int py = 0;
                bool put = false;
                // Placed in the middle of the edited chunk, so the
                // profile above is squarely the chunk that owns it.
                const int edit_col_x = edited_x + world::kChunkSizeX / 2;
                const int edit_col_z = ez + world::kChunkSizeZ / 2;
                for (int i = 0; i < 64 && !put; ++i) {
                    py = 80 + i;
                    put = wrld.set_block(edit_col_x, py, edit_col_z,
                                         world::BlockId::Glow);
                }
                if (!put) {
                    std::fprintf(stderr, "[verify-4d] edit_rotates could not "
                                 "place its block at (%d,*,%d) - window is "
                                 "radius %d\n", edit_col_x, edit_col_z,
                                 opt.stream_radius);
                }
                settle_slice();
                std::vector<int> e0, c0, e1, c1;
                profile(edited_x, ez, e0);
                profile(control_x, ez, c0);
                for (int i = 0; i < 60; ++i) wrld.rotate_slice(kNotch, 0.0f);
                settle_slice();
                profile(edited_x, ez, e1);
                profile(control_x, ez, c1);
                const int moved_edited = differing(e0, e1);
                const int moved_control = differing(c0, c1);
                // The edit itself must also still be there: replaying it
                // over regenerated terrain is the whole mechanism, and a
                // version that let the chunk rotate by dropping the edit
                // would pass a terrain-only check.
                const bool still_there =
                    wrld.block_at(edit_col_x, py, edit_col_z)
                        == world::BlockId::Glow;
                // A quarter of the footprint is far above the noise floor
                // and far below what a full rotation moves; the measured
                // values are most of the 256 columns in both chunks.
                constexpr int kMoved = 64;
                edit_rotates = put && still_there &&
                               moved_edited > kMoved && moved_control > kMoved;
                if (!edit_rotates) {
                    std::fprintf(stderr, "[verify-4d] edit_rotates: placed=%d "
                                 "still_there=%d edited moved %d/256 "
                                 "control moved %d/256\n",
                                 put ? 1 : 0, still_there ? 1 : 0,
                                 moved_edited, moved_control);
                }
                for (int i = 0; i < 60; ++i) wrld.rotate_slice(-kNotch, 0.0f);
                settle_slice();
                wrld.set_block(edit_col_x, py, edit_col_z, world::BlockId::Air);
                settle_slice();
            }

            // Rotation must be EXACTLY reversible away from the origin,
            // which is the case the tilt phase above cannot see.
            //
            // That phase rotates with player_z = 0 at w = 0, where the
            // offset o is 0 and every formula returns 0 to 0 whatever it
            // does in between. It passed while a notch out and a notch
            // back left o at o*cos^2(delta) - so at any w != 0, scrolling
            // to and fro slid the player along the fourth axis without
            // them touching a travel key: 8.5% of their w after ten
            // thousand notches, HUD counter drifting to match. A check
            // that only ever runs where the quantity it guards is zero is
            // not a check.
            const float rev_w0 = wrld.slice_w();
            const float rev_shift0 = wrld.slice().z_shift;
            constexpr float kRevPlayerZ = 640.0f;   // well away from 0
            for (int i = 0; i < 40; ++i) wrld.rotate_slice(kNotch, kRevPlayerZ);
            for (int i = 0; i < 40; ++i) wrld.rotate_slice(-kNotch, kRevPlayerZ);
            // The tolerance is 1e-4 and cannot usefully be tighter: at
            // player_z = 640, eighty float32 rotations of a value that
            // size accumulate about 1e-5 of rounding, and that is
            // arithmetic rather than a defect.
            //
            // Which is exactly why the edit namespace must not be derived
            // from this number. A residue a hundred times smaller than
            // this tolerance was enough to move floor(slice_w_) from 0 to
            // -1 and strand every edit in the world; the check would have
            // passed while it happened. edit_slice() reads travel_w_ now,
            // and edit_ns_stable pins that separately.
            const bool reversible_away =
                std::fabs(wrld.slice_w() - rev_w0) < 1e-4f &&
                std::fabs(wrld.slice().z_shift - rev_shift0) < 1e-3f &&
                std::fabs(wrld.slice_theta() - theta0) < 1e-4f;
            settle_slice();

            // An edit made WHILE a rebuild is in flight must survive.
            //
            // Every other edit check settles first, so none of them place
            // a block at the one moment a player most often does: while
            // scrolling the wheel or holding a travel key, when jobs are
            // in flight for the very chunk being edited.
            //
            // The stale-result guard cannot catch this, and that is worth
            // stating because it looks like the guard's job. The job was
            // issued BEFORE the edit, so its stamp still matches and the
            // result is accepted - carrying a copy of the replay list
            // from submit time, without the new edit. Measured before the
            // fix: idle placement survived, placement at 24 jobs in
            // flight did not.
            bool edit_survives_inflight = false;
            {
                travel_to(w0);
                settle_slice();
                // Make the whole window stale, then edit without draining
                // - this is the in-flight state, not a simulation of it.
                for (int i = 0; i < 8; ++i) wrld.rotate_slice(kNotch, 0.0f);
                wrld.stream_slice(terrain, pool, 64);
                const int inflight = wrld.pending_async();
                int rx = 4, ry = 0, rz = 4;
                bool placed_racing = false;
                for (int i = 0; i < 64 && !placed_racing; ++i) {
                    ry = 76 + i;
                    placed_racing =
                        wrld.set_block(rx, ry, rz, world::BlockId::Glow);
                }
                settle_slice();
                const bool still =
                    wrld.block_at(rx, ry, rz) == world::BlockId::Glow;
                edit_survives_inflight = placed_racing && inflight > 0 && still;
                if (!edit_survives_inflight) {
                    std::fprintf(stderr, "[verify-4d] an edit placed with %d "
                                 "jobs in flight was lost (placed=%d)\n",
                                 inflight, placed_racing ? 1 : 0);
                }
                wrld.set_block(rx, ry, rz, world::BlockId::Air);
                for (int i = 0; i < 8; ++i) wrld.rotate_slice(-kNotch, 0.0f);
                settle_slice();
            }

            // A tilt must survive travelling along w.
            //
            // Every hash check in this mode runs at theta = 0 and every
            // rotation check runs at w = 0, so the two axes were never
            // combined and nothing noticed a travel that silently threw
            // the tilt away. Setting slice_theta_ = 0 at the top of
            // resample_slice passed the whole suite with every field 1.
            bool tilt_survives_travel = false;
            {
                const float t0 = wrld.slice_theta();
                for (int i = 0; i < 30; ++i) wrld.rotate_slice(kNotch, 0.0f);
                const float tilted = wrld.slice_theta();
                travel_to(w0 + 1.0f);
                const float after_out = wrld.slice_theta();
                travel_to(w0);
                const float after_back = wrld.slice_theta();
                tilt_survives_travel =
                    std::fabs(tilted - t0) > 1e-4f &&
                    std::fabs(after_out - tilted) < 1e-4f &&
                    std::fabs(after_back - tilted) < 1e-4f;
                if (!tilt_survives_travel) {
                    std::fprintf(stderr, "[verify-4d] travel changed the tilt: "
                                 "%.4f -> %.4f -> %.4f\n",
                                 tilted, after_out, after_back);
                }
                for (int i = 0; i < 30; ++i) wrld.rotate_slice(-kNotch, 0.0f);
                settle_slice();
            }

            // Turning about the player must leave the player's own
            // terrain EXACTLY where it was.
            //
            // This is the pivot's other half, and it had no coverage at
            // all: pivot_ok compares staleness COUNTS through
            // slice_drift, and slice_w_ still moves with player_z in
            // rotate_slice, so the counts stay comparable even when the
            // sampling is wrong. Deleting z_shift from to_4d - the line
            // that actually carries the pivot into the generator - passed
            // the entire suite with pivot=1, while the ground under the
            // player slid out from under them:
            //
            //     player's own row   pristine 0/128 moved, fault 69/128
            //     control row        pristine 103/128,     fault 106/128
            //
            // So this reads the terrain, not the bookkeeping.
            bool player_ground_fixed = false;
            {
                const int pz = 400;                  // away from the origin
                const int cz = (pz / world::kChunkSizeZ) * world::kChunkSizeZ;
                wrld.update_streaming(
                    world::ChunkCoord{0, cz / world::kChunkSizeZ}, 4,
                    terrain, pool);
                settle_slice();
                auto row = [&](std::vector<int>& out) {
                    out.clear();
                    for (int x = 0; x < world::kChunkSizeX; ++x) {
                        int top = 0;
                        for (int y = world::kChunkSizeY - 1; y > 0; --y)
                            if (wrld.block_at(x, y, cz) != world::BlockId::Air) {
                                top = y; break;
                            }
                        out.push_back(top);
                    }
                };
                std::vector<int> before, after;
                row(before);
                const float pivot_z =
                    static_cast<float>(cz) + world::kChunkSizeZ / 2.0f;
                for (int i = 0; i < 20; ++i) wrld.rotate_slice(kNotch, pivot_z);
                settle_slice();
                row(after);
                int moved = 0;
                for (std::size_t i = 0; i < before.size(); ++i)
                    if (before[i] != after[i]) ++moved;
                // Not a tolerance: the player's own row is the fixed point
                // of the rotation, so it must not move at all.
                player_ground_fixed = (moved == 0);
                if (!player_ground_fixed) {
                    std::fprintf(stderr, "[verify-4d] the ground under the "
                                 "player moved under a rotation about them: "
                                 "%d of %zu columns\n", moved, before.size());
                }
                for (int i = 0; i < 20; ++i) wrld.rotate_slice(-kNotch, pivot_z);
                wrld.update_streaming(world::ChunkCoord{0, 0}, opt.stream_radius,
                                      terrain, pool);
                settle_slice();
            }

            // The wheel must do the same thing wherever the player is
            // standing, and that is a property of the PIVOT rather than
            // of the rotation.
            //
            // Turning the slice about the world origin makes the effect
            // scale with how far the player has walked, because a tilt
            // displaces a point in proportion to its distance from the
            // axis. Measured at one notch, before the pivot moved to the
            // player: 7.0% of columns changed at spawn, 62.7% after a
            // hundred seconds of walking, 95.8% far out - and the row at
            // z=0 was exactly invariant at every angle, so at spawn,
            // looking down the x axis, the wheel did nothing at all.
            //
            // Checked through staleness rather than through content,
            // because staleness is what the engine acts on: one notch
            // must leave the chunks NEAREST the player alone, and that
            // has to stay true a long way from the origin. With the pivot
            // at the origin every chunk around a distant player is far
            // from the axis, so all of them go stale at once.
            auto near_stale_after_a_notch = [&](int centre_chunk_z) {
                const world::ChunkCoord centre{0, centre_chunk_z};
                wrld.update_streaming(centre, 4, terrain, pool);
                settle_slice();
                const float pz = static_cast<float>(
                    centre_chunk_z * world::kChunkSizeZ + world::kChunkSizeZ / 2);
                wrld.rotate_slice(kNotch, pz);
                const auto lag = wrld.slice_lag();
                wrld.rotate_slice(-kNotch, pz);
                settle_slice();
                return lag;
            };
            const auto near_home = near_stale_after_a_notch(0);
            const auto near_away = near_stale_after_a_notch(400);   // 6400 blocks
            // Some chunks must be spared in both places - the wheel is a
            // fine control, not a full rebuild - and the two must be
            // comparable, which is the part that fails when the pivot is
            // in the wrong place.
            const bool pivot_ok =
                near_home.stale < near_home.resident &&
                near_away.stale < near_away.resident &&
                near_away.stale <= near_home.stale * 2 + 2;
            wrld.update_streaming(world::ChunkCoord{0, 0}, opt.stream_radius,
                                  terrain, pool);
            settle_slice();

            // No resident chunk may be left permanently stale.
            //
            // The claim this used to make - that it catches
            // enqueue_decoded_chunk failing to stamp the slice - is
            // false, and was checked: deleting that stamping still passes
            // the whole suite. Stale drains monotonically under that
            // fault because a chunk already meshed against a neighbour is
            // not re-dirtied, so the bad stamp lands once per pair and
            // never bounces back.
            //
            // What it does pin is narrower and real: that stream_slice's
            // SELECTION covers its own staleness predicate. slice_lag
            // counts every resident chunk; held_geometry only looks
            // within four chunks of the camera, and `settled` only
            // asserts stream_slice returned nothing more to ask for. When
            // selection and predicate diverge - a chunk stale but never
            // re-requested, from starvation, a budget bug, an ordering
            // bug, a skip at the window edge - this is the only field
            // with global reach. Verified by starving the far ring:
            // `converges` fired alone, with settled=1 and ground_fixed=1
            // beside it.
            wrld.rotate_slice(kNotch * 3.0f, 0.0f);
            settle_slice();
            const auto after_rotate = wrld.slice_lag();
            const bool converges = after_rotate.stale == 0;
            wrld.rotate_slice(-kNotch * 3.0f, 0.0f);
            settle_slice();

            const bool tilt_ok = !settle_timed_out &&
                                 edit_survives_scroll && converges &&
                                 pivot_ok && player_ground_fixed &&
                                 tilt_survives_travel &&
                                 edit_survives_inflight &&
                                 reversible_away &&
                                 edit_rotates && namespace_stable &&
                                 hash_tilt != hash_w0 &&
                                 hash_untilt == hash_w0 &&
                                 bad_tilt == 0 && bad_untilt == 0 &&
                                 notch_moves_world &&
                                 std::fabs(wrld.slice_theta() - theta0) < 1e-6f;

            // Simulated held key: the interactive path, driven exactly as
            // the render loop drives it - move_w once per frame with a
            // frame's worth of dt, then the same per-frame drain the loop
            // does. Everything above tests one big jump; this tests the
            // thing the player actually does, and it is the only check
            // that would notice the throttle refusing every rebuild.
            travel_to(w0);
            const float held_w0 = wrld.near_meshed_w();
            int rebuilds = 0;
            constexpr int kFrames = 120;          // two seconds at 60 Hz
            constexpr float kDt = 1.0f / 60.0f;
            for (int f = 0; f < kFrames; ++f) {
                const auto frame_end = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(16667);
                // Exactly what the render loop does: advance the player,
                // pull a bounded slice of the world toward them, drain.
                // This used to call move_w, which is the whole-window
                // rebuild the interactive path no longer uses - so the
                // check was passing on a code path the player never
                // touches.
                wrld.advance_w(world::World::kWalkSpeedW * kDt);
                if (wrld.stream_slice(terrain, pool) > 0) ++rebuilds;
                // The render loop's own adaptive budget, copied exactly.
                // Mirroring it matters: with a flat 16 the check requested
                // 24 chunks a frame and uploaded 16, so the queue grew all
                // run and the world could never catch up. That is a
                // property of the test, not of the engine, and it made a
                // large radius look broken.
                wrld.drain_finished(wrld.pending_async() > 32 ? 48 : 16);
                wrld.flush_pending_remeshes(pool, 4);
                // Real frame pacing, and it is load-bearing rather than
                // cosmetic. Without it this loop ran all 120 iterations in
                // microseconds while claiming a 1/60 dt, so the worker
                // pool never got wall-clock time to finish a single
                // rebuild - in-flight sat at a full window forever and the
                // check reported the geometry frozen. That looked exactly
                // like the engine bug it was meant to find, and cost a
                // round of tuning the wrong constant.
                while (std::chrono::steady_clock::now() < frame_end) {
                    std::this_thread::yield();
                }
            }
            const float held_travelled = wrld.slice_w() - w0;
            // The near field, not the global worst. At a large radius the
            // most-stale chunk is past the fog and its lag says more about
            // window size than about what the player sees - judging by it
            // failed this check at radius 12 while the visible world was
            // entirely current.
            const float held_geometry  = wrld.near_meshed_w() - held_w0;
            // Two seconds of walking must move the player and must move
            // the geometry with them. Geometry may lag by up to one
            // rebuild, so it is checked as a fraction rather than exactly.
            const bool held_ok = rebuilds > 0 &&
                                 held_travelled > 0.5f &&
                                 held_geometry > held_travelled * 0.5f;

            // Building across the fourth dimension: an edit made on one
            // slice must survive travelling away and coming back, and must
            // NOT appear on the slice next door.
            //
            // Both halves matter. Without the first, nothing you build
            // persists and w is a sightseeing axis. Without the second, a
            // hole dug into a hillside at w=0 turns up in mid-air at w=5,
            // where the terrain around it means something else entirely.
            travel_to(w0);
            int ex = 0, ey = 0, ez = 0;
            bool placed = false;
            for (int i = 0; i < 256 && !placed; ++i) {
                ex = (i * 7) % 32;
                ey = 70 + (i % 8);
                ez = (i * 11) % 32;
                placed = wrld.set_block(ex, ey, ez, world::BlockId::Glow);
            }
            settle();
            const bool edit_here = wrld.block_at(ex, ey, ez) == world::BlockId::Glow;
            travel_to(w0 + 3.0f);
            const bool absent_away =
                wrld.block_at(ex, ey, ez) != world::BlockId::Glow;
            travel_to(w0);
            const bool back_again =
                wrld.block_at(ex, ey, ez) == world::BlockId::Glow;
            const bool edit_ok = placed && edit_here && absent_away && back_again;

            const bool ok = edit_ok && held_ok && tilt_ok && requested > 0 &&
                            hash_w1 != hash_w0 &&      // w is a real axis
                            hash_back == hash_w0 &&    // and a reversible one
                            position_ok &&
                            bad_w0 == 0 && bad_w1 == 0 &&
                            bad_back == 0;

            std::printf("\nVERIFY4D w=%.2f chunks=%d step_ms=%.1f "
                        "changed=%d returned=%d "
                        "held_rebuilds=%d held_travelled=%.2f held_geometry=%.2f "
                        "edit_survives_w=%d tilt_changed=%d tilt_returned=%d "
                        "notch=%d edit_survives_scroll=%d converges=%d "
                        "pivot=%d ground_fixed=%d tilt_survives_travel=%d "
                        "edit_survives_inflight=%d "
                        "reversible_away=%d "
                        "edit_rotates=%d settled=%d "
                        "edit_ns_stable=%d "
                        "bad_tris=%d/%d/%d %s\n",
                        w0, requested, step_ms,
                        hash_w1 != hash_w0 ? 1 : 0,
                        hash_back == hash_w0 ? 1 : 0,
                        rebuilds, held_travelled, held_geometry,
                        edit_ok ? 1 : 0,
                        hash_tilt != hash_w0 ? 1 : 0,
                        (hash_untilt == hash_w0 && bad_tilt == 0
                         && bad_untilt == 0) ? 1 : 0,
                        notch_moves_world ? 1 : 0,
                        edit_survives_scroll ? 1 : 0,
                        converges ? 1 : 0,
                        pivot_ok ? 1 : 0,
                        player_ground_fixed ? 1 : 0,
                        tilt_survives_travel ? 1 : 0,
                        edit_survives_inflight ? 1 : 0,
                        reversible_away ? 1 : 0,
                        edit_rotates ? 1 : 0,
                        settle_timed_out ? 0 : 1,
                        namespace_stable ? 1 : 0,
                        bad_w0, bad_w1, bad_back,
                        ok ? "ok" : "FAILED");
            if (!ok) return EXIT_FAILURE;
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // --bench-4d: what does moving through the fourth dimension cost?
        //
        // The engine's other benches measure a STATIC world - how fast it
        // draws, how much memory the meshes take. Neither says anything
        // about the thing that makes a 4D engine hard, which is that
        // moving along the fourth axis invalidates geometry rather than
        // just moving the camera through it. A step along w changes what
        // is in every chunk; a rotation changes it more.
        //
        // So this measures the two motions the same way, under the same
        // 60 Hz pacing the render loop uses, and reports what each costs
        // per unit of world change rather than per unit of input. Input
        // units are not comparable: 0.003 rad and 0.003 w are wildly
        // different amounts of new world (63.8% of columns against under
        // 1%), so a bench that reported "ms per radian" against "ms per w"
        // would be measuring two different things and inviting the
        // comparison anyway.
        if (opt.bench_4d && world_settled) {
            struct Phase { const char* name; float w_rate; float theta_rate; };
            // Rates a player can actually produce. 0.4 w/s is the walk
            // speed along w; 0.18 rad/s is a steady scroll, about 60
            // notches a second.
            // Rates a player can actually produce, and a range of them
            // for the wheel rather than one number.
            //
            // A single rotation rate was misleading. The bench used 0.18
            // rad/s, which is sixty scroll notches a second - a rate no
            // hand sustains - and reported the whole window permanently
            // stale, which said more about the chosen rate than about the
            // engine. The useful question is where the wheel stops
            // outrunning the stream, and that needs a sweep.
            //
            // 0.003 rad is one notch, so the rates below are 1, 5, 15 and
            // 60 notches a second. Fifteen is a brisk deliberate turn;
            // sixty is a trackpad flick.
            const Phase phases[] = {
                {"translate",  world::World::kWalkSpeedW,  0.0f},
                {"sprint w",   world::World::kSprintSpeedW, 0.0f},
                {"scroll 1/s",  0.0f, 0.003f},
                {"scroll 5/s",  0.0f, 0.015f},
                {"scroll 15/s", 0.0f, 0.045f},
                {"scroll 60/s", 0.0f, 0.180f},
            };
            constexpr int   kFrames = 300;        // five seconds each
            constexpr float kDt = 1.0f / 60.0f;
            // World::stream_slice's own default, named here so the report
            // can say what the issued count is being measured against.
            constexpr int   kStreamBudget = 24;

            std::printf("\n4D motion cost, radius %d, %d frames per phase "
                        "at 60 Hz\n\n", opt.stream_radius, kFrames);
            // No chunks/sec column, deliberately. The first version had
            // one and it reported 1430 for translation and 1439 for
            // rotation - which is 24 x 60, the per-frame streaming budget
            // times the frame rate, to three digits. Both phases saturate
            // the budget every frame, so that column was reporting the
            // CONSTANT and would have been quoted as a measured
            // throughput. What the budget leaves is reported instead:
            // whether the world keeps up at it, and how long it takes to
            // converge once the motion stops.
            std::printf("  %-11s %10s %10s %12s %12s %12s\n", "motion",
                        "mean ms", "p99 ms", "issued/frame", "behind",
                        "settle ms");

            for (const Phase& ph : phases) {
                // Start each phase from the SAME slice, not just a
                // converged one.
                //
                // The phases run in sequence and each one moves the
                // world: translate and sprint leave w at 8 between them.
                // A rotation about the player displaces distant terrain
                // in proportion to the slice's offset, so the scroll rows
                // were measuring their own motion plus however far the
                // travel rows had already gone - the 60/s row read 255 ms
                // to settle against 187 for sprinting, and most of that
                // gap was inherited w rather than the wheel. Resetting
                // makes the rows comparable, which is the only reason to
                // put them in one table.
                wrld.set_slice_source(&terrain4d, 0.0f);
                const auto warm_t0 = std::chrono::steady_clock::now();
                bool warmed = false;
                const auto warm_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (std::chrono::steady_clock::now() < warm_deadline) {
                    wrld.drain_finished(256);
                    wrld.flush_pending_remeshes(pool, 256);
                    if (wrld.pending_async() == 0 && wrld.pending_remesh() == 0
                        && wrld.stream_slice(terrain, pool, 256) == 0) {
                        warmed = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                // Only when it fails, and it must not fail: a phase that
                // starts from a half-built world measures the warm-up
                // rather than the motion. This loop streams 256 chunks an
                // iteration rather than the default 24 for exactly that
                // reason - it breaks only when nothing is in flight AND
                // nothing is stale in the same pass, and at 24 a
                // freshly-reset 625-chunk world almost never satisfies
                // both at once. It timed out, and the scroll rows read
                // 4 ms a frame and two seconds to settle: the reset, not
                // the wheel.
                if (!warmed) {
                    std::fprintf(stderr, "[warm] %s did not converge in %.0f "
                                 "ms, %d chunks still stale - the row below "
                                 "measures the warm-up, not the motion\n",
                                 ph.name,
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - warm_t0)
                                     .count(),
                                 wrld.slice_lag().stale);
                }
                std::vector<double> frame_ms;
                frame_ms.reserve(kFrames);
                int chunks = 0;

                const auto phase_t0 = std::chrono::steady_clock::now();
                for (int f = 0; f < kFrames; ++f) {
                    const auto frame_end = std::chrono::steady_clock::now() +
                        std::chrono::microseconds(16667);
                    const auto t0 = std::chrono::steady_clock::now();
                    // Exactly the render loop's per-frame slice work.
                    if (ph.w_rate != 0.0f) wrld.advance_w(ph.w_rate * kDt);
                    if (ph.theta_rate != 0.0f)
                        wrld.rotate_slice(ph.theta_rate * kDt, 0.0f);
                    chunks += wrld.stream_slice(terrain, pool);
                    wrld.drain_finished(wrld.pending_async() > 32 ? 48 : 16);
                    wrld.flush_pending_remeshes(pool, 4);
                    frame_ms.push_back(
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count());
                    // Real pacing, for the same reason --verify-4d needs
                    // it: without wall-clock time between frames the nine
                    // workers never run, and the main thread measures
                    // itself waiting on work that has not started.
                    while (std::chrono::steady_clock::now() < frame_end)
                        std::this_thread::yield();
                }
                const double elapsed_s =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - phase_t0).count();

                // How much of the resident world was behind the current
                // slice at the moment the motion stopped. Captured HERE,
                // before the settle loop below, or it would report a
                // converged world every time and always read 0.
                //
                // The first version reported a w lag from near_meshed_w,
                // and it read 0.000 for every rotate phase - not because
                // rotation is free but because rotating does not change
                // slice_w, so a w-based lag is zero by construction. It
                // was a tautology in a results column. This counts stale
                // chunks, which means the same thing on both axes.
                const auto lag = wrld.slice_lag();

                // Then: how long to converge once the motion STOPS. This
                // is the number a player feels as the world catching up,
                // and it is the one a whole-world rebuild would blow out.
                const auto settle_t0 = std::chrono::steady_clock::now();
                // Whether it actually converged, or ran out of guard.
                //
                // This is reported rather than assumed because the
                // difference was invisible once: with the slice-stamp
                // defect, restored and re-meshed chunks landed claiming
                // slice (0, 0), so they were instantly stale again and
                // this loop never converged - it exhausted the guard and
                // reported the time that took, which looked like a settle
                // time and was published as one. A settle that times out
                // must not be able to masquerade as a fast settle.
                // Bounded by TIME, not by iteration count, and this is
                // the second time that distinction has bitten in this
                // file. An iteration guard is a spin count: the body is a
                // couple of microseconds when there is nothing finished to
                // drain, so 4000 of them elapse in 8 ms while the nine
                // workers have barely started. The loop then exits with
                // most of the window still stale, having measured how long
                // it takes to spin 4000 times - which is a number, is
                // stable, looks like a settle time, and is not one.
                bool converged = false;
                const auto settle_deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (std::chrono::steady_clock::now() < settle_deadline) {
                    wrld.drain_finished(48);
                    wrld.flush_pending_remeshes(pool, 4);
                    if (wrld.pending_async() == 0 && wrld.pending_remesh() == 0
                        && wrld.stream_slice(terrain, pool) == 0) {
                        converged = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (!converged) {
                    const auto l = wrld.slice_lag();
                    std::fprintf(stderr, "[settle] %s did not converge: "
                                 "stale=%d/%d async=%d remesh=%d issued=%d\n",
                                 ph.name, l.stale, l.resident,
                                 wrld.pending_async(), wrld.pending_remesh(),
                                 wrld.stream_slice(terrain, pool));
                }
                const double settle_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - settle_t0).count();

                std::sort(frame_ms.begin(), frame_ms.end());
                double sum = 0.0;
                for (const double v : frame_ms) sum += v;
                const double mean = sum / static_cast<double>(frame_ms.size());
                const double p99 = frame_ms[static_cast<std::size_t>(
                    (frame_ms.size() - 1) * 0.99)];
                const double issued_per_frame =
                    static_cast<double>(chunks) / kFrames;
                char behind[32];
                std::snprintf(behind, sizeof behind, "%d / %d",
                              lag.stale, lag.resident);
                char settle[24];
                if (converged) {
                    std::snprintf(settle, sizeof settle, "%.0f", settle_ms);
                } else {
                    std::snprintf(settle, sizeof settle, "NEVER (%.0f)",
                                  settle_ms);
                }
                std::printf("  %-11s %10.2f %10.2f %6.1f / %-5d %12s %12s\n",
                            ph.name, mean, p99, issued_per_frame, kStreamBudget,
                            behind, settle);
                (void)elapsed_s;
            }
            std::printf("\nmain-thread cost only; chunk generation and "
                        "meshing run on the %d-worker pool.\n"
                        "issued/frame at the budget means the motion "
                        "invalidates geometry faster than the stream\n"
                        "replaces it, so the figures to read are behind - "
                        "how much of the resident world was\nstale when the "
                        "motion stopped - and settle, how long it then took "
                        "to converge.\n",
                        static_cast<int>(pool.worker_count()));
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Headless --verify-edit-persistence: prove a block edit survives
        // its chunk being streamed out and back in. Break a solid block
        // near the origin, recenter the stream window far away (edited
        // chunk evicts -> stash), recenter home (chunk restores from the
        // stash, not the terrain generator), then check the hole is still
        // there. All on the main thread, deterministic.
        if (verify_edit_persistence && world_settled) {
            int ex = 0, ey = 0, ez = 0;
            world::BlockId prev = world::BlockId::Air;
            for (int probe = 0; probe < 4096 && prev == world::BlockId::Air;
                 ++probe) {
                ex = (probe * 37) % 16;
                ez = (probe * 53) % 16;
                ey = 20 + (probe % 30);
                prev = wrld.block_at(ex, ey, ez);
            }
            bool ok = prev != world::BlockId::Air;
            world::World::StreamStats away{}, back{};
            bool evicted = false;
            bool survived_in_session = false;
            if (ok) {
                wrld.set_block(ex, ey, ez, world::BlockId::Air);
                const world::ChunkCoord home{0, 0};
                // 3 radii away: no overlap between the two windows, so the
                // edited chunk cannot ride along in the resident set.
                const world::ChunkCoord far_off{home.x + 3 * stream_radius, home.z};
                away = wrld.update_streaming(far_off, stream_radius, terrain, pool);
                while (wrld.pending_async() > 0) wrld.drain_finished(64);
                evicted = !wrld.has_chunk(home);
                back = wrld.update_streaming(home, stream_radius, terrain, pool);
                while (wrld.pending_async() > 0) wrld.drain_finished(64);
                // has_chunk guards the survival check: block_at reports Air
                // for an unloaded chunk too, which would pass vacuously.
                // The edit must come back, and it must come back through
                // the REPLAY path rather than by the chunk being handed
                // back whole.
                //
                // This used to assert stashed >= 1 && restored >= 1,
                // which pinned the mechanism instead of the outcome - and
                // the mechanism was the defect: a stashed chunk is
                // restored verbatim, so it stops being generated at all,
                // and once the slice could rotate that froze the chunk
                // against every further turn of the wheel. Whole-chunk
                // stashing is now only for chunks that came off disk,
                // which the generator genuinely cannot reproduce.
                // Either mechanism is correct, and which one applies
                // says something real about the world's provenance:
                //
                //   generated   the chunk is regenerated and the edit is
                //               replayed on top, so it keeps changing
                //               with the slice
                //   from disk   the generator cannot reproduce it, so it
                //               is stashed whole and handed back verbatim
                //
                // Asserting exactly one of them fires is what stops this
                // passing on a world that quietly took the wrong path.
                const bool via_replay = back.replayed >= 1 &&
                                        away.stashed == 0 &&
                                        back.restored == 0;
                const bool via_stash  = away.stashed >= 1 &&
                                        back.restored >= 1 &&
                                        back.replayed == 0;
                ok = evicted && (via_replay != via_stash) &&
                     wrld.has_chunk(home) &&
                     wrld.block_at(ex, ey, ez) == world::BlockId::Air;
                // Captured HERE, not read again at print time. The two
                // legs below deliberately move the world - one reloads it
                // from disk, the other wipes it - so a field read at the
                // end reports whatever those left behind rather than what
                // this leg measured. It printed survived=0 on a passing
                // run for exactly that reason.
                survived_in_session =
                    wrld.block_at(ex, ey, ez) == world::BlockId::Air;
            }

            // And it must reach DISK from out of view.
            //
            // The check above only proves the edit comes back within the
            // session. When edits moved from a whole-chunk stash to a
            // replay list, save_world kept writing the stash and nothing
            // wrote the replay list, so an edit made outside the stream
            // window was simply absent from the save file - dig a hole,
            // walk past the radius, save, reload, and the hole is gone.
            // In-session everything looked right, which is why this needs
            // its own leg rather than an extra assertion on the one above.
            bool survives_disk = false;
            if (ok) {
                const world::ChunkCoord home{0, 0};
                const world::ChunkCoord far_off{home.x + 3 * stream_radius,
                                                home.z};
                // Evict it again, so the save has to find it somewhere
                // other than the resident set.
                wrld.update_streaming(far_off, stream_radius, terrain, pool);
                while (wrld.pending_async() > 0) wrld.drain_finished(64);
                const std::string dir = "/tmp/voxel_edit_disk_check";
                std::filesystem::remove_all(dir);
                const auto saved = world::save_world(wrld, dir, terrain_seed);
                wrld.clear_all();
                const auto loaded = world::load_world(wrld, dir, pool,
                                                      terrain_seed);
                while (wrld.pending_async() > 0) wrld.drain_finished(64);
                survives_disk = saved.ok && loaded.ok &&
                                wrld.has_chunk(home) &&
                                wrld.block_at(ex, ey, ez) == world::BlockId::Air;
                std::filesystem::remove_all(dir);
                if (!survives_disk) {
                    std::fprintf(stderr, "[edit-persist] save/load lost the "
                                 "edit: saved=%d loaded=%d resident=%d\n",
                                 saved.ok ? 1 : 0, loaded.ok ? 1 : 0,
                                 wrld.has_chunk(home) ? 1 : 0);
                }
                ok = survives_disk;
            }

            // And a wipe must actually wipe.
            //
            // clear_all() clears the resident chunks and the whole-chunk
            // stash, but the replay list was added later and was not
            // added here - so request_terrain_chunk replayed the
            // DISCARDED world's edits over the new one's terrain. F6 and
            // --bench-io both reach it.
            //
            // This needs its own leg because the save/load check above
            // cannot see it: there the edit is supposed to come back, so
            // a leak and a correct load look identical. Here nothing is
            // loaded, so the only right answer is the terrain the
            // generator makes.
            bool wipe_is_clean = false;
            if (ok) {
                const world::ChunkCoord home{0, 0};
                wrld.clear_all();
                wrld.update_streaming(home, stream_radius, terrain, pool);
                while (wrld.pending_async() > 0) wrld.drain_finished(64);
                wipe_is_clean = wrld.has_chunk(home) &&
                                wrld.block_at(ex, ey, ez) == prev;
                if (!wipe_is_clean) {
                    std::fprintf(stderr, "[edit-persist] a cleared world kept "
                                 "the old one's edit: expected %d, got %d\n",
                                 static_cast<int>(prev),
                                 static_cast<int>(wrld.block_at(ex, ey, ez)));
                }
                ok = wipe_is_clean;
            }
            std::printf("\nEDIT_PERSIST block=(%d,%d,%d) prev_id=%d evicted=%d "
                        "stashed=%d restored=%d replayed=%d survived=%d "
                        "survives_disk=%d wipe_clean=%d %s\n",
                        ex, ey, ez, static_cast<int>(prev),
                        evicted ? 1 : 0, away.stashed, back.restored,
                        back.replayed,
                        survived_in_session ? 1 : 0,
                        survives_disk ? 1 : 0,
                        wipe_is_clean ? 1 : 0,
                        ok ? "ok" : "FAILED");
            if (!ok) {
                return EXIT_FAILURE;
            }
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Headless --bench-edit: once the world is resident, hammer set_block
        // with deterministic break-then-restore edit pairs spread across
        // chunks, timing each full synchronous edit (greedy remesh +
        // section re-bucket + GL upload + visibility recompute), then print
        // one BENCH_EDIT distribution line and exit. Restoring the original
        // block keeps the world unchanged between pairs.
        if (bench_edit > 0 && world_settled) {
            std::vector<double> edit_samples;
            edit_samples.reserve(static_cast<std::size_t>(bench_edit));
            int probe = 0;
            // Positions walk a deterministic lattice over a 7x7-chunk
            // neighborhood at underground depths that are solid on any
            // seed's terrain, so break edits never no-op.
            while (static_cast<int>(edit_samples.size()) < bench_edit &&
                   probe < bench_edit * 64) {
                const int x = ((probe * 37) % 112) - 56;
                const int z = ((probe * 53) % 112) - 56;
                const int y = 20 + (probe % 30);
                ++probe;
                const world::BlockId prev = wrld.block_at(x, y, z);
                if (prev == world::BlockId::Air) continue;
                const auto t0 = std::chrono::steady_clock::now();
                wrld.set_block(x, y, z, world::BlockId::Air);
                edit_samples.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count());
                if (static_cast<int>(edit_samples.size()) < bench_edit) {
                    const auto t1 = std::chrono::steady_clock::now();
                    wrld.set_block(x, y, z, prev);
                    edit_samples.push_back(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t1).count());
                } else {
                    wrld.set_block(x, y, z, prev);  // restore untimed
                }
            }
            if (edit_samples.empty()) {
                std::fprintf(stderr, "--bench-edit found no solid blocks to edit\n");
                return EXIT_FAILURE;
            }
            std::vector<double> sorted = edit_samples;
            std::sort(sorted.begin(), sorted.end());
            const std::size_t n = sorted.size();
            double sum = 0.0;
            for (double s : sorted) sum += s;
            const double p50 = sorted[n / 2];
            const double p99 = sorted[std::min<std::size_t>(n - 1,
                static_cast<std::size_t>(static_cast<double>(n) * 0.99))];
            std::printf("\nBENCH_EDIT edits=%zu avg_ms=%.3f p50_ms=%.3f "
                        "p99_ms=%.3f max_ms=%.3f radius=%d\n",
                        n, sum / static_cast<double>(n), p50, p99,
                        sorted[n - 1], stream_radius);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Scripted screenshot: scene only (pre-HUD), after the world finished
        // loading plus shot_after settle frames. Fixed filename makes runs
        // pixel-diffable (occlusion on/off A/B).
        if (shot_after > 0 && world_settled && --shot_after == 0) {
            if (std::getenv("VOXEL_VALIDATE")) wrld.debug_dump_visibility(view_frustum);
            const std::string path =
                gfx::save_screenshot(fb_w, fb_h, "./screenshots", shot_file);
            std::printf("[screenshot] %s  (pose=%.*s occlusion=%s sections=%d)\n",
                        path.empty() ? "FAILED" : path.c_str(),
                        static_cast<int>(bench_pose.size()), bench_pose.data(),
                        occlusion_cull_enabled ? "on" : "off",
                        last_stats.sections_drawn);
            std::fflush(stdout);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        hud.begin_frame();
        ui::PerfFrame pf;
        pf.frame_ms        = smoothed_frame_ms;
        pf.fps             = smoothed_fps;
        pf.chunks_total    = last_stats.chunks_total;
        pf.chunks_drawn    = last_stats.chunks_drawn;
        pf.sections_drawn    = last_stats.sections_drawn;
        pf.sections_occluded = last_stats.sections_occluded;
        pf.occlusion_enabled = occlusion_cull_enabled;
        pf.place_block_name  = block_name(place_id);
        pf.ai_texture_tiles  = ai_texture_tiles;
        pf.triangles_drawn = last_stats.triangles_drawn;
        pf.gpu_bytes       = wrld.resident_gpu_bytes();
        pf.stash_chunks    = wrld.stash_count();
        pf.stash_bytes     = wrld.stash_bytes();
        pf.pending_async   = wrld.pending_async();
        pf.initial_load_ms = initial_load_ms;
        pf.total_chunks    = total_chunks;
        pf.worker_count    = worker_count;
        pf.streamed_in     = streamed_in_total;
        pf.streamed_out    = streamed_out_total;
        pf.four_d          = wrld.is_4d();
        pf.slice_w         = wrld.slice_w();
        pf.slice_theta     = wrld.slice_theta();
        pf.slice_phi       = wrld.slice_phi();
        pf.meshed_w        = wrld.near_meshed_w();
        pf.edit_count      = wrld.edit_count();
        pf.edit_last_ms    = wrld.edit_last_ms();
        pf.edit_avg_ms     = wrld.edit_avg_ms();
        pf.edit_max_ms     = wrld.edit_max_ms();
        hud.draw_perf_panel(pf);
        if (copy_perf_requested) hud.copy_perf_to_clipboard(pf);
        hud.end_frame_and_render();

        glfwSwapBuffers(window);
        glfwPollEvents();
        FrameMark;

        ++frame_count;
        ++frame_index;

        sampler.set_settled(world_settled);
        if (sampler.record(static_cast<double>(dt) * 1000.0,
                           last_stats.triangles_drawn)) {
            bench::print_frame_report({
                .stream_radius   = stream_radius,
                .pose            = bench_pose,
                .total_chunks    = total_chunks,
                .stats           = sampler.stats(),
                .triangles_sum   = sampler.triangles_sum(),
                .chunks_drawn    = last_stats.chunks_drawn,
                .sections_drawn  = last_stats.sections_drawn,
                .triangles_drawn = last_stats.triangles_drawn,
                .gpu_buffers_mb  = static_cast<double>(wrld.resident_gpu_bytes())
                                   / (1024.0 * 1024.0),
            });
            if (bench_pass_breakdown) {
                bench::print_pass_breakdown(sampler.passes());
            }
            std::fflush(stdout);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        if (now - last_time >= 1.0) {
            char title[256];
            std::snprintf(title, sizeof(title),
                "voxel_engine  |  %d fps  |  pos %.0f %.0f %.0f  |  chunks %d/%d  |  tris %zu  |  pending %d",
                frame_count,
                cam.position().x, cam.position().y, cam.position().z,
                last_stats.chunks_drawn, last_stats.chunks_total,
                last_stats.triangles_drawn,
                wrld.pending_async());
            glfwSetWindowTitle(window, title);
            frame_count = 0;
            last_time = now;
        }
    }

    hud.shutdown();
    if (sky_vao)       glDeleteVertexArrays(1, &sky_vao);
    if (crosshair_vao) glDeleteVertexArrays(1, &crosshair_vao);
    if (block_atlas)   glDeleteTextures(1, &block_atlas);
    return EXIT_SUCCESS;  // window_guard tears down GLFW after the GL objects
}
