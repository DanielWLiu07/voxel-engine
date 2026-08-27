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

// Shared save/load console report: both directions measure and print
// identically, so the two lines stay comparable at a glance.
void print_io_report(const char* verb, int chunks, double ms,
                     std::size_t bytes_disk, std::size_t bytes_raw,
                     bool ok) {
    const double ratio = bytes_disk > 0
        ? static_cast<double>(bytes_raw) / bytes_disk : 0.0;
    const double secs = ms / 1000.0;
    const double mb_disk = bytes_disk / (1024.0 * 1024.0);
    const double mb_raw  = bytes_raw  / (1024.0 * 1024.0);
    std::printf("[%s] %s %d chunks in %.1f ms  |  "
                "%.2f MB on disk vs %.2f MB raw  |  %.1fx ratio  |  "
                "%.0f MB/s disk, %.0f MB/s raw  |  %s\n",
                verb, verb[0] == 's' ? "wrote" : "read", chunks, ms,
                mb_disk, mb_raw, ratio,
                secs > 0.0 ? mb_disk / secs : 0.0,
                secs > 0.0 ? mb_raw  / secs : 0.0,
                ok ? "ok" : "ERRORS");
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
                     const world::World& wrld, bool walk_mode) {
    cam.apply_mouse_delta(input.mouse_dx(), input.mouse_dy(), 0.12f);

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
    const int thread_override = opt.thread_override;
    const int orbit_frames = opt.orbit_frames;
    const int cycle_frames = opt.cycle_frames;
    // The two capture questions, named once (core/capture_mode.h). Built
    // from the same values the locals above carry; shot_after is the one
    // that counts down, so `capture` is rebuilt where that matters.
    core::CaptureMode capture{shot_after, orbit_frames, cycle_frames,
                              bench_frames};
    const bool no_occlusion = opt.no_occlusion;
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
                          !save_path.empty();
    bool vsync_enabled = (bench_frames == 0 && shot_after == 0);
    auto win = core::Window::create({.visible = !headless,
                                     .vsync   = vsync_enabled});
    if (!win) return EXIT_FAILURE;
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
    cam.set_position({0.0f, 80.0f, 80.0f});
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

    core::Input input;
    input.attach(window);
    input.set_cursor_captured(true);

    ui::DebugHud hud;
    if (!hud.init(window)) {
        std::fprintf(stderr, "imgui init failed\n");
        return EXIT_FAILURE;
    }

    game::Player player;
    player.set_position({0.0f, 80.0f, 0.0f});
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

        input.begin_frame();

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
            auto t0 = std::chrono::steady_clock::now();
            auto s = world::save_world(wrld, kSaveDir, terrain_seed);
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            print_io_report("save", s.chunks_written, ms,
                            s.bytes_written, s.bytes_raw, s.ok);
        }
        if (input.key_pressed(core::key_of(core::Bind::Load))) {
            auto t0 = std::chrono::steady_clock::now();
            wrld.clear_all();
            auto l = world::load_world(wrld, kSaveDir, pool, terrain_seed);
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            print_io_report("load", l.chunks_read, ms,
                            l.bytes_read, l.bytes_raw,
                            l.ok && l.files_skipped == 0);
            if (l.files_skipped > 0) {
                std::fprintf(stderr, "[load] WARNING: %d chunk file%s corrupt "
                             "or unreadable, skipped\n",
                             l.files_skipped, l.files_skipped == 1 ? "" : "s");
            }
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
        capture.shot_after = shot_after;  // counts down as the shot settles
        if (input.cursor_captured() && !capture.scripted_camera()) {
            update_movement(input, dt, cam, player, wrld, walk_mode);
            handle_block_interaction(input, cam, player, walk_mode, wrld, place_id);
        }

        // Scripted captures drive the camera themselves. Both step by frame
        // index, not dt, so a slow frame cannot put a hitch into the
        // assembled clip. The orbit flies a fixed-step circle at constant
        // height, always looking at the scene center; the cycle parks at
        // the orbit's start pose and spends the frames on one full day of
        // time-of-day instead.
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
        wrld.drain_finished(16);
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
        if (wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        last_stats = render::draw_terrain(shaders.terrain, shadow_map, wrld, fv, light,
                                          kBlockPalette, view_frustum,
                                          occlusion_cull_enabled);
        if (wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        sampler.end_pass(sampler.passes().terrain);
        if (!sky_overdraw) {
            sampler.begin_pass();
            render::draw_sky(shaders.sky, sky_vao, fv, light, true);
            sampler.end_pass(sampler.passes().sky);
        }
        sampler.begin_pass();
        render::draw_water(shaders.water, water, fv, light,
                           static_cast<float>(world::kSeaLevel));
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

        // Headless --validate: once the world is resident, read every mesh
        // back off the GPU, check each triangle against the voxel data, and
        // exit nonzero on offenders. Composes with --load to verify a saved
        // world and with --seed to spot-check other maps.
        // Wait for a settled world: chunks meshed before their neighbours
        // arrived are still owed a re-mesh, and validating (or measuring)
        // mid-convergence reports the pre-culling footprint.
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

        if (validate_mode && world_settled) {
            const int bad = wrld.debug_validate_gpu_meshes();
            // The engine's own resident mesh footprint, printed here
            // because this is the only headless mode that builds a real
            // world on a real GPU. --bench computes the same figure from
            // the mesher alone; the two agreeing is what says the
            // streaming path is uploading what the mesher produces.
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
                ok = evicted && away.stashed >= 1 && back.restored >= 1 &&
                     wrld.has_chunk(home) &&
                     wrld.block_at(ex, ey, ez) == world::BlockId::Air;
            }
            std::printf("\nEDIT_PERSIST block=(%d,%d,%d) prev_id=%d evicted=%d "
                        "stashed=%d restored=%d survived=%d %s\n",
                        ex, ey, ez, static_cast<int>(prev),
                        evicted ? 1 : 0, away.stashed, back.restored,
                        wrld.block_at(ex, ey, ez) == world::BlockId::Air ? 1 : 0,
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
