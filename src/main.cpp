#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "core/input.h"
#include "core/profiler.h"
#include "core/thread_pool.h"
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
#include "ui/debug_hud.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/terrain_gen.h"
#include "world/world.h"
#include "world/world_io.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr float kFlySpeed       = 16.0f;
constexpr float kFlySprintSpeed = 60.0f;
constexpr int   kStreamRadius   = 12;
[[maybe_unused]] constexpr const char* kSaveDir = "./saves/world1";
constexpr float kWaterSize      = 480.0f;
constexpr int   kWaterSubdiv    = 200;
constexpr int   kShadowMapSize  = 2048;
constexpr float kShadowNear     = 0.1f;
constexpr float kShadowFar      = 250.0f;

const glm::vec3 kBlockPalette[8] = {
    {1.00f, 0.00f, 1.00f},  // Air (never seen)
    {0.55f, 0.55f, 0.58f},  // Stone
    {0.50f, 0.34f, 0.20f},  // Dirt
    {0.34f, 0.62f, 0.27f},  // Grass
    {0.88f, 0.80f, 0.55f},  // Sand
    {0.42f, 0.27f, 0.13f},  // Wood
    {0.22f, 0.46f, 0.20f},  // Leaves
    {0.95f, 0.96f, 0.98f},  // Snow
};

void glfw_error(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}

void framebuffer_resize(GLFWwindow*, int w, int h) {
    glViewport(0, 0, w, h);
}

fs::path find_asset_root(const char* argv0) {
    fs::path start = fs::absolute(argv0).parent_path();
    for (fs::path p = start; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "shaders" / "basic.vert")) return p;
        if (p == p.root_path()) break;
    }
    return fs::current_path();
}

int run_bench() {
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
        std::size_t naive_tris  = last_naive.indices.size()  / 3;
        std::size_t greedy_tris = last_greedy.indices.size() / 3;

        std::printf("---- %s ----\n", label);
        std::printf("naive : quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                    last_naive.quad_count, naive_tris, naive_total / kRuns);
        std::printf("greedy: quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                    last_greedy.quad_count, greedy_tris, greedy_total / kRuns);
        if (last_greedy.quad_count > 0 && greedy_tris > 0) {
            std::printf("ratio : %.1fx fewer quads  |  %.1fx fewer tris\n",
                        static_cast<double>(last_naive.quad_count)  / last_greedy.quad_count,
                        static_cast<double>(naive_tris)             / greedy_tris);
        }
    };

    std::printf("==== chunk mesher benchmark (%d runs, Perlin terrain chunk 0,0) ====\n", kRuns);
    // Caves-off measures the greedy algorithm against contiguous terrain
    // — this is what the CI gate checks. Caves-on is the realistic
    // gameplay path; lower ratio is expected because caves break up
    // mergeable face runs.
    bench_one(/*caves=*/false, "contiguous terrain (CI gate)");
    bench_one(/*caves=*/true,  "with caves (gameplay terrain)");

    // ---- Frustum cull benchmark ---------------------------------------
    // CPU-only, deterministic. Generates a 25x25 chunk grid and counts how
    // many AABBs the view frustum keeps, under four (AABB, far-plane)
    // combinations so the improvement from tightening either dimension is
    // visible side-by-side. No GL context needed.
    constexpr int kRadius = kStreamRadius;
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
    wide_aabbs.reserve(total);
    tight_aabbs.reserve(total);
    section_bounds.reserve(total);
    int total_sections_nonempty = 0;
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
            auto secs = world::compute_section_bounds({cx, cz}, c);
            for (const auto& s : secs) if (s.has_mesh) ++total_sections_nonempty;
            section_bounds.push_back(std::move(secs));
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
    auto count_sections_visible = [&](float zfar) {
        gfx::Frustum f;
        f.from_view_proj(cam.proj_matrix(kAspect, kFovDeg, 0.1f, zfar) * view);
        int drawn = 0;
        for (std::size_t i = 0; i < tight_aabbs.size(); ++i) {
            if (!f.intersects_aabb(tight_aabbs[i])) continue;
            for (const auto& s : section_bounds[i]) {
                if (s.has_mesh && f.intersects_aabb(s.aabb)) ++drawn;
            }
        }
        return drawn;
    };
    const int sections_drawn = count_sections_visible(kFarTight);

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

    // Stable, machine-readable summary line so CI can gate the cull ratios
    // without fishing through the prose. Whitespace-separated key=value
    // pairs after a fixed prefix.
    std::printf("\nBENCH_SUMMARY"
                " chunk_tight=%.2f"
                " section_nonempty=%.2f"
                " section_total=%.2f\n",
                ratio(tight_fartight, total),
                ratio(sections_drawn, total_sections_nonempty),
                ratio(sections_drawn, total * world::kSectionsPerChunk));
    return EXIT_SUCCESS;
}

bool load_shader(gfx::Shader& s, const fs::path& root,
                 const char* vert, const char* frag, const char* tag) {
    if (s.load((root / "shaders" / vert).string(),
               (root / "shaders" / frag).string())) return true;
    std::fprintf(stderr, "[shader] %s load failed\n", tag);
    return false;
}

void handle_block_interaction(core::Input& input,
                              const gfx::FlyCamera& cam,
                              const game::Player& player,
                              bool walk_mode,
                              world::World& wrld) {
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
    wrld.set_block(px, py, pz, world::BlockId::Stone);
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

}  // namespace

int main(int argc, char** argv) {
    // --bench (CPU mesher + cull bench) returns early.
    // --bench-frame N (GL frame-time bench) opens a hidden window, locks the
    // camera to a fixed pose, runs N frames vsync-off, prints a stable
    // BENCH_FRAME line and exits. Same renderer the gameplay uses.
    int bench_frames = 0;
    bool bench_pass_breakdown = false;
    std::string_view bench_pose = "center";
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::printf(
                "voxel_engine - desktop voxel engine, C++20 / OpenGL 4.1\n"
                "\n"
                "Usage:\n"
                "  voxel_engine                          launch the gameplay window\n"
                "  voxel_engine --bench                  CPU mesher + cull bench (no GL window)\n"
                "  voxel_engine --bench-frame N          run N vsync-off frames, print BENCH_FRAME\n"
                "  voxel_engine --bench-frame N --pose P bench at named pose (center, ground, high)\n"
                "  voxel_engine --bench-frame N --pass-breakdown\n"
                "                                        wall time per render pass (glFinish-bracketed)\n"
                "  voxel_engine --help                   this text\n"
                "\n"
                "See README.md for the reproducible perf tables and CI gates.\n");
            return EXIT_SUCCESS;
        }
        if (arg == "--bench") return run_bench();
        if (arg == "--bench-frame" && i + 1 < argc) {
            bench_frames = std::atoi(argv[i + 1]);
            ++i;
        }
        if (arg == "--pass-breakdown") bench_pass_breakdown = true;
        if (arg == "--pose" && i + 1 < argc) {
            bench_pose = argv[i + 1];
            ++i;
        }
    }

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 2);
    if (bench_frames > 0) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "voxel_engine", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    bool vsync_enabled = (bench_frames == 0);
    glfwSwapInterval(vsync_enabled ? 1 : 0);

    int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        glfwDestroyWindow(window); glfwTerminate();
        return EXIT_FAILURE;
    }
    std::printf("GL %d.%d  |  vendor=%s  |  renderer=%s\n",
                GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version),
                glGetString(GL_VENDOR), glGetString(GL_RENDERER));

    glfwSetFramebufferSizeCallback(window, framebuffer_resize);
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_MULTISAMPLE);

    fs::path root = find_asset_root(argv[0]);
    std::printf("[boot] asset root = %s\n", root.string().c_str());

    gfx::Shader shader, sky_shader, shadow_shader, water_shader;
    gfx::Shader bright_shader, blur_shader, tonemap_shader;
    gfx::Shader wireframe_shader, crosshair_shader;
    if (!load_shader(shader,           root, "basic.vert",        "basic.frag",         "terrain")   ||
        !load_shader(sky_shader,       root, "sky.vert",          "sky.frag",           "sky")       ||
        !load_shader(shadow_shader,    root, "shadow_depth.vert", "shadow_depth.frag",  "shadow")    ||
        !load_shader(water_shader,     root, "water.vert",        "water.frag",         "water")     ||
        !load_shader(bright_shader,    root, "fullscreen.vert",   "bright_extract.frag","bright")    ||
        !load_shader(blur_shader,      root, "fullscreen.vert",   "blur_separable.frag","blur")      ||
        !load_shader(tonemap_shader,   root, "fullscreen.vert",   "tonemap.frag",       "tonemap")   ||
        !load_shader(wireframe_shader, root, "wireframe.vert",    "wireframe.frag",     "wireframe") ||
        !load_shader(crosshair_shader, root, "crosshair.vert",    "crosshair.frag",     "crosshair")) {
        glfwDestroyWindow(window); glfwTerminate();
        return EXIT_FAILURE;
    }

    gfx::PostProcess postfx;
    if (!postfx.init(fb_w, fb_h)) {
        std::fprintf(stderr, "post-process init failed\n");
        glfwDestroyWindow(window); glfwTerminate();
        return EXIT_FAILURE;
    }
    int postfx_w = fb_w, postfx_h = fb_h;
    std::printf("[postfx] HDR %dx%d + half-res bloom chain allocated\n", fb_w, fb_h);

    gfx::CascadedShadowMap shadow_map;
    if (!shadow_map.init(kShadowMapSize)) {
        glfwDestroyWindow(window); glfwTerminate();
        return EXIT_FAILURE;
    }
    std::printf("[shadow] %dx%d depth map allocated\n", kShadowMapSize, kShadowMapSize);

    gfx::WaterPlane water;
    if (!water.init(kWaterSize, kWaterSubdiv)) {
        glfwDestroyWindow(window); glfwTerminate();
        return EXIT_FAILURE;
    }
    std::printf("[water] %.0fx%.0f plane (sea level y=%d, follows player)\n",
                kWaterSize, kWaterSize, world::kSeaLevel);

    GLuint sky_vao = 0;
    glGenVertexArrays(1, &sky_vao);

    // Procedural texture atlas for blocks. Generated once at boot.
    GLuint block_atlas = gfx::generate_block_atlas();
    std::printf("[atlas] %dx%d procedural block atlas\n",
                gfx::kAtlasSizePx, gfx::kAtlasSizePx);
    GLuint crosshair_vao = 0;
    glGenVertexArrays(1, &crosshair_vao);

    gfx::WireframeCube selection_cube;
    selection_cube.init();

    const std::size_t worker_count = std::max<std::size_t>(2,
        std::thread::hardware_concurrency() - 1);
    world::TerrainGen terrain(1337);
    world::World wrld;
    core::ThreadPool pool(worker_count);

    const int total_chunks = (2 * kStreamRadius + 1) * (2 * kStreamRadius + 1);
    std::printf("[world] streaming %d chunks (radius=%d) onto %zu workers\n",
                total_chunks, kStreamRadius, worker_count);

    auto async_t0 = std::chrono::steady_clock::now();
    wrld.enqueue_grid_async(kStreamRadius, terrain, pool);

    bool   initial_load_logged = false;
    double initial_load_ms     = 0.0;
    world::ChunkCoord last_center{0, 0};
    int streamed_in_total  = 0;
    int streamed_out_total = 0;

    gfx::FlyCamera cam;
    cam.set_position({0.0f, 80.0f, 80.0f});
    cam.set_yaw_pitch(-90.0f, -35.0f);
    if (bench_frames > 0) {
        // Named poses keep the perf table reproducible across vantage
        // points. "center" matches the --bench cull pose for direct
        // comparability with the cull-ratio table; "ground" is an
        // eye-level walk pose; "high" is a top-down vantage that
        // exercises the section-AABB cull's vertical pruning.
        if (bench_pose == "ground") {
            cam.set_position({0.0f, 35.0f, 0.0f});
            cam.set_yaw_pitch(-90.0f, 0.0f);
        } else if (bench_pose == "high") {
            cam.set_position({0.0f, 150.0f, 0.0f});
            cam.set_yaw_pitch(-90.0f, -45.0f);
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
        glfwDestroyWindow(window); glfwTerminate();
        return EXIT_FAILURE;
    }

    game::Player player;
    player.set_position({0.0f, 80.0f, 0.0f});
    bool walk_mode = false;

    float time_of_day = 0.35f;
    const float day_speed = 1.0f / 240.0f;
    // Bench mode pauses time-of-day so a sunrise/sunset transition mid-bench
    // can't fire the shadow-resync force-refresh path and inject a spike.
    bool  time_paused = (bench_frames > 0);

    std::printf("[input] WASD = move, Space = jump (walk) / up (fly), LCtrl = down (fly)\n");
    std::printf("[input] LClick = break, RClick = place, Shift = sprint\n");
    std::printf("[input] F = toggle walk/fly, Tab = mouse capture, F2 = HUD, ESC = quit\n");
    std::printf("[input] T = pause time, [/] = step time, V = toggle vsync\n");
    std::printf("[input] F5 = save world, F6 = load world (./saves/world1)\n");
    std::printf("[input] F12 = screenshot (./screenshots)\n");

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

    // --bench-frame state: collected per-frame after the initial chunk load
    // settles plus a short warmup, so the samples reflect steady-state
    // rendering rather than the streaming ramp or first-frame GL state
    // transitions.
    std::vector<double> bench_samples;
    if (bench_frames > 0) bench_samples.reserve(static_cast<std::size_t>(bench_frames));
    // 30 settle frames after initial_load_logged is generous (~200 ms at
    // typical bench frame times) but cleanly clears post-load shader
    // re-jit, driver buffer-orphan settling, and the cascade-warmup spike
    // that was still surfacing in the radius-8 center-pose tail with a
    // 10-frame settle.
    constexpr int kBenchSettleFrames = 30;
    int bench_settle_remaining = kBenchSettleFrames;

    // --pass-breakdown state. Each per-pass accumulator captures one entry
    // per frame after initial_load_logged becomes true. glFinish bracketing
    // forces the GPU to drain before timing, so these reflect actual
    // dispatch+execution wall time rather than CPU command-submission only;
    // the trade-off is that the frame-level avg_ms in this mode is slightly
    // inflated by the synchronization itself.
    std::vector<double> pass_ms_shadow, pass_ms_sky, pass_ms_terrain,
                        pass_ms_water,  pass_ms_postfx;
    if (bench_pass_breakdown) {
        const std::size_t reserve_n = bench_frames > 0
            ? static_cast<std::size_t>(bench_frames) : 1024;
        pass_ms_shadow.reserve(reserve_n);
        pass_ms_sky.reserve(reserve_n);
        pass_ms_terrain.reserve(reserve_n);
        pass_ms_water.reserve(reserve_n);
        pass_ms_postfx.reserve(reserve_n);
    }
    std::chrono::steady_clock::time_point pass_t0{};
    auto pass_start = [&](void) {
        if (bench_pass_breakdown && initial_load_logged) {
            glFinish();
            pass_t0 = std::chrono::steady_clock::now();
        }
    };
    auto pass_end = [&](std::vector<double>& acc) {
        if (bench_pass_breakdown && initial_load_logged) {
            glFinish();
            acc.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pass_t0).count());
        }
    };

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = static_cast<float>(now - prev_frame_time);
        prev_frame_time = now;

        input.begin_frame();

        smoothed_frame_ms = smoothed_frame_ms * 0.9f + (dt * 1000.0f) * 0.1f;
        float instant_fps = (dt > 0.0f) ? (1.0f / dt) : 0.0f;
        smoothed_fps = smoothed_fps * 0.9f + instant_fps * 0.1f;

        if (input.key_down(GLFW_KEY_ESCAPE))    glfwSetWindowShouldClose(window, GLFW_TRUE);
        if (input.key_pressed(GLFW_KEY_TAB))    input.set_cursor_captured(!input.cursor_captured());
        if (input.key_pressed(GLFW_KEY_F2))     hud.toggle_visible();
        if (input.key_pressed(GLFW_KEY_F12)) {
            std::string path = gfx::save_screenshot(fb_w, fb_h);
            if (!path.empty()) std::printf("[screenshot] %s\n", path.c_str());
        }
        if (input.key_pressed(GLFW_KEY_T))      time_paused = !time_paused;
        if (input.key_pressed(GLFW_KEY_V)) {
            vsync_enabled = !vsync_enabled;
            glfwSwapInterval(vsync_enabled ? 1 : 0);
            std::printf("[gfx] vsync %s\n", vsync_enabled ? "on" : "off");
        }
        if (input.key_pressed(GLFW_KEY_F5)) {
            auto t0 = std::chrono::steady_clock::now();
            auto s = world::save_world(wrld, kSaveDir);
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            double ratio = s.bytes_written > 0
                ? static_cast<double>(s.bytes_raw) / s.bytes_written : 0.0;
            const double secs = ms / 1000.0;
            const double mb_disk = s.bytes_written / (1024.0 * 1024.0);
            const double mb_raw  = s.bytes_raw     / (1024.0 * 1024.0);
            const double disk_mbps = secs > 0.0 ? mb_disk / secs : 0.0;
            const double raw_mbps  = secs > 0.0 ? mb_raw  / secs : 0.0;
            std::printf("[save] wrote %d chunks in %.1f ms  |  "
                        "%.2f MB on disk vs %.2f MB raw  |  %.1fx ratio  |  "
                        "%.0f MB/s disk, %.0f MB/s raw  |  %s\n",
                        s.chunks_written, ms,
                        mb_disk, mb_raw, ratio,
                        disk_mbps, raw_mbps,
                        s.ok ? "ok" : "ERRORS");
        }
        if (input.key_pressed(GLFW_KEY_F6)) {
            auto t0 = std::chrono::steady_clock::now();
            wrld.clear_all();
            auto l = world::load_world(wrld, kSaveDir, terrain);
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            double ratio = l.bytes_read > 0
                ? static_cast<double>(l.bytes_raw) / l.bytes_read : 0.0;
            const double secs = ms / 1000.0;
            const double mb_disk = l.bytes_read / (1024.0 * 1024.0);
            const double mb_raw  = l.bytes_raw  / (1024.0 * 1024.0);
            const double disk_mbps = secs > 0.0 ? mb_disk / secs : 0.0;
            const double raw_mbps  = secs > 0.0 ? mb_raw  / secs : 0.0;
            std::printf("[load] read %d chunks in %.1f ms  |  "
                        "%.2f MB on disk vs %.2f MB raw  |  %.1fx ratio  |  "
                        "%.0f MB/s disk, %.0f MB/s raw  |  %s\n",
                        l.chunks_read, ms,
                        mb_disk, mb_raw, ratio,
                        disk_mbps, raw_mbps,
                        l.ok ? "ok" : "ERRORS");
            // Reset streaming bookkeeping so the next move triggers a refill
            // around the player for anything missing on disk.
            last_center = world::ChunkCoord{
                static_cast<std::int32_t>(std::floor(cam.position().x / world::kChunkSizeX)) + 1,
                last_center.z};
        }
        if (input.key_down(GLFW_KEY_RIGHT_BRACKET)) time_of_day += dt * 0.05f;
        if (input.key_down(GLFW_KEY_LEFT_BRACKET))  time_of_day -= dt * 0.05f;
        if (!time_paused) time_of_day += dt * day_speed;
        time_of_day -= std::floor(time_of_day);

        bool copy_perf_requested = input.key_pressed(GLFW_KEY_C);
        if (input.key_pressed(GLFW_KEY_F)) {
            walk_mode = !walk_mode;
            if (walk_mode) {
                player.set_position(cam.position()
                                    - glm::vec3(0.0f, game::Player::kEyeHeight, 0.0f));
            }
            std::printf("[mode] %s\n", walk_mode ? "walk" : "fly");
        }

        if (input.cursor_captured()) {
            update_movement(input, dt, cam, player, wrld, walk_mode);
            handle_block_interaction(input, cam, player, walk_mode, wrld);
        }

        world::ChunkCoord center{
            static_cast<std::int32_t>(std::floor(cam.position().x / world::kChunkSizeX)),
            static_cast<std::int32_t>(std::floor(cam.position().z / world::kChunkSizeZ))
        };
        if (initial_load_logged && !(center == last_center)) {
            auto sstats = wrld.update_streaming(center, kStreamRadius, terrain, pool);
            streamed_in_total  += sstats.requested;
            streamed_out_total += sstats.evicted;
            last_center = center;
        }
        wrld.drain_finished(16);
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
        }

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w != postfx_w || fb_h != postfx_h) {
            postfx.init(fb_w, fb_h);
            postfx_w = fb_w;
            postfx_h = fb_h;
        }
        float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;

        render::FrameView fv;
        fv.view       = cam.view_matrix();
        fv.camera_pos = cam.position();
        fv.window_w   = fb_w;
        fv.window_h   = fb_h;
        fv.fog_end    = static_cast<float>(kStreamRadius * world::kChunkSizeX) * 0.95f;
        fv.fog_start  = fv.fog_end * 0.78f;  // less aggressive midrange wash-out
        // Camera far plane sits just past the fog plane: anything further is
        // fully fogged out and contributes nothing. Tightening it from the
        // 500 m default also gives the frustum a real far-plane cull instead
        // of one that never trips at radius 12.
        const float kCameraFar = fv.fog_end + static_cast<float>(world::kChunkSizeX);
        fv.proj       = cam.proj_matrix(aspect, 70.0f, 0.1f, kCameraFar);
        fv.time_seconds = static_cast<float>(now);

        render::LightingFrame light = render::compute_lighting(time_of_day);

        // Stagger: refresh cascade c only every (1 << c) frames. The far
        // cascade is hundreds of meters wide and barely changes frame to
        // frame, so paying 3x shadow cost to refresh near-stale data is
        // wasted work. c1 and c2 are phased so they never coincide with
        // each other — peak passes/frame stays at 2 instead of 3, keeping
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
        // skip-pass — force-refresh all cascades to resync.
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
        pass_start();
        render::draw_shadow_pass(shadow_map, shadow_shader, wrld, fv, light,
                                 shadow_cascade_mask);
        pass_end(pass_ms_shadow);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, block_atlas);

        postfx.begin_scene();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        pass_start();
        render::draw_sky(sky_shader, sky_vao, fv, light);
        pass_end(pass_ms_sky);
        pass_start();
        last_stats = render::draw_terrain(shader, shadow_map, wrld, fv, light,
                                          kBlockPalette, view_frustum);
        pass_end(pass_ms_terrain);
        pass_start();
        render::draw_water(water_shader, water, fv, light,
                           static_cast<float>(world::kSeaLevel));
        pass_end(pass_ms_water);

        // Same ray the place/break logic uses, so the outline matches a
        // potential click target.
        auto target = wrld.raycast(cam.position(), cam.forward(), 8.0f);
        render::draw_crosshair_and_selection(
            wireframe_shader, selection_cube,
            crosshair_shader, crosshair_vao,
            fv,
            target.hit,
            target.block_x, target.block_y, target.block_z);

        // HDR -> bright extract -> blur -> ACES tonemap to backbuffer.
        pass_start();
        postfx.resolve_to_backbuffer(bright_shader, blur_shader, tonemap_shader,
                                     fb_w, fb_h,
                                     /*blur_iter*/ 4,
                                     /*threshold*/ 1.0f,
                                     /*intensity*/ 0.7f,
                                     /*exposure*/  1.0f);
        pass_end(pass_ms_postfx);

        hud.begin_frame();
        ui::PerfFrame pf;
        pf.frame_ms        = smoothed_frame_ms;
        pf.fps             = smoothed_fps;
        pf.chunks_total    = last_stats.chunks_total;
        pf.chunks_drawn    = last_stats.chunks_drawn;
        pf.triangles_drawn = last_stats.triangles_drawn;
        pf.pending_async   = wrld.pending_async();
        pf.initial_load_ms = initial_load_ms;
        pf.total_chunks    = total_chunks;
        pf.worker_count    = worker_count;
        pf.streamed_in     = streamed_in_total;
        pf.streamed_out    = streamed_out_total;
        hud.draw_perf_panel(pf);
        if (copy_perf_requested) hud.copy_perf_to_clipboard(pf);
        hud.end_frame_and_render();

        glfwSwapBuffers(window);
        glfwPollEvents();
        FrameMark;

        ++frame_count;
        ++frame_index;

        if (bench_frames > 0 && initial_load_logged) {
            if (bench_settle_remaining > 0) { --bench_settle_remaining; }
            else bench_samples.push_back(static_cast<double>(dt) * 1000.0);
            if (static_cast<int>(bench_samples.size()) >= bench_frames) {
                std::vector<double> sorted = bench_samples;
                std::sort(sorted.begin(), sorted.end());
                const std::size_t n = sorted.size();
                double sum = 0.0;
                for (double v : sorted) sum += v;
                const double avg = sum / static_cast<double>(n);
                const double p50 = sorted[n / 2];
                const double p99 = sorted[std::min<std::size_t>(n - 1,
                                            static_cast<std::size_t>(n * 0.99))];
                const double mn  = sorted.front();
                const double mx  = sorted.back();
                // Peak RSS. ru_maxrss is bytes on macOS, kilobytes on Linux.
                struct rusage ru{};
                getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
                const double peak_mb = static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
#else
                const double peak_mb = static_cast<double>(ru.ru_maxrss) / 1024.0;
#endif
                const double tris_per_sec = (avg > 0.0)
                    ? static_cast<double>(last_stats.triangles_drawn) * 1000.0 / avg
                    : 0.0;

                std::printf("\nBENCH_FRAME"
                            " radius=%d pose=%.*s chunks=%d frames=%zu"
                            " avg_ms=%.2f p50_ms=%.2f p99_ms=%.2f"
                            " min_ms=%.2f max_ms=%.2f avg_fps=%.1f"
                            " drawn_chunks=%d drawn_sections=%d tris=%zu"
                            " tris_per_sec=%.0f peak_rss_mb=%.1f\n",
                            kStreamRadius,
                            static_cast<int>(bench_pose.size()), bench_pose.data(),
                            total_chunks, n,
                            avg, p50, p99, mn, mx,
                            (avg > 0.0 ? 1000.0 / avg : 0.0),
                            last_stats.chunks_drawn,
                            last_stats.sections_drawn,
                            last_stats.triangles_drawn,
                            tris_per_sec, peak_mb);
                if (bench_pass_breakdown && !pass_ms_shadow.empty()) {
                    auto mean = [](const std::vector<double>& v) {
                        double s = 0.0; for (double x : v) s += x;
                        return v.empty() ? 0.0 : s / static_cast<double>(v.size());
                    };
                    const double s_sh = mean(pass_ms_shadow);
                    const double s_sk = mean(pass_ms_sky);
                    const double s_te = mean(pass_ms_terrain);
                    const double s_wa = mean(pass_ms_water);
                    const double s_pf = mean(pass_ms_postfx);
                    std::printf("PASS_BREAKDOWN frames=%zu"
                                " shadow=%.2f sky=%.2f terrain=%.2f"
                                " water=%.2f postfx=%.2f sum_passes=%.2f\n",
                                pass_ms_shadow.size(),
                                s_sh, s_sk, s_te, s_wa, s_pf,
                                s_sh + s_sk + s_te + s_wa + s_pf);
                }
                std::fflush(stdout);
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
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
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
