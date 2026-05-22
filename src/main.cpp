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

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <thread>

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
    {1.00f, 0.00f, 1.00f},
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
    world::TerrainGen terrain(1337);
    world::Chunk chunk;
    terrain.fill_chunk(0, 0, chunk);

    constexpr int kRuns = 25;
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

    std::printf("==== chunk mesher benchmark (%d runs, Perlin terrain chunk 0,0) ====\n", kRuns);
    std::printf("naive : quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                last_naive.quad_count, naive_tris, naive_total / kRuns);
    std::printf("greedy: quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                last_greedy.quad_count, greedy_tris, greedy_total / kRuns);
    if (last_greedy.quad_count > 0 && greedy_tris > 0) {
        std::printf("ratio : %.1fx fewer quads  |  %.1fx fewer tris\n",
                    static_cast<double>(last_naive.quad_count)  / last_greedy.quad_count,
                    static_cast<double>(naive_tris)             / greedy_tris);
    }
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
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--bench") return run_bench();
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

    GLFWwindow* window = glfwCreateWindow(1280, 720, "voxel_engine", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    bool vsync_enabled = true;
    glfwSwapInterval(1);

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
    bool  time_paused = false;

    std::printf("[input] WASD = move, Space = jump (walk) / up (fly), LCtrl = down (fly)\n");
    std::printf("[input] LClick = break, RClick = place, Shift = sprint\n");
    std::printf("[input] F = toggle walk/fly, Tab = mouse capture, F2 = HUD, ESC = quit\n");
    std::printf("[input] T = pause time, [/] = step time, V = toggle vsync\n");
    std::printf("[input] F5 = save world, F6 = load world, F12 = screenshot (./screenshots)\n");
    std::printf("[input] F5 = save world, F6 = load world (./saves/world1)\n");

    double last_time = glfwGetTime();
    double prev_frame_time = glfwGetTime();
    int    frame_count = 0;
    world::DrawStats last_stats{};
    float smoothed_fps      = 0.0f;
    float smoothed_frame_ms = 0.0f;

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
            std::printf("[save] wrote %d chunks in %.1f ms  |  "
                        "%.2f MB on disk vs %.2f MB raw  |  %.1fx ratio  |  %s\n",
                        s.chunks_written, ms,
                        s.bytes_written / (1024.0 * 1024.0),
                        s.bytes_raw     / (1024.0 * 1024.0),
                        ratio,
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
            std::printf("[load] read %d chunks in %.1f ms  |  "
                        "%.2f MB on disk vs %.2f MB raw  |  %.1fx ratio  |  %s\n",
                        l.chunks_read, ms,
                        l.bytes_read / (1024.0 * 1024.0),
                        l.bytes_raw  / (1024.0 * 1024.0),
                        ratio,
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
        fv.proj       = cam.proj_matrix(aspect);
        fv.camera_pos = cam.position();
        fv.window_w   = fb_w;
        fv.window_h   = fb_h;
        fv.fog_end    = static_cast<float>(kStreamRadius * world::kChunkSizeX) * 0.95f;
        fv.fog_start  = fv.fog_end * 0.55f;
        fv.time_seconds = static_cast<float>(now);

        render::LightingFrame light = render::compute_lighting(time_of_day);
        auto cascades = gfx::CascadedShadowMap::fit_cascades(
            fv.view, fv.proj, light.sun_dir, kShadowNear, kShadowFar);
        for (int c = 0; c < gfx::kNumCascades; ++c) {
            fv.light_vp[c]     = cascades[c].light_vp;
            fv.cascade_far[c]  = cascades[c].split_far_view;
        }

        gfx::Frustum view_frustum;
        view_frustum.from_view_proj(fv.proj * fv.view);

        // Shadow pass writes to its own FBO; the other scene passes write
        // into the HDR FBO via begin_scene().
        render::draw_shadow_pass(shadow_map, shadow_shader, wrld, fv, light);

        postfx.begin_scene();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        render::draw_sky(sky_shader, sky_vao, fv, light);
        last_stats = render::draw_terrain(shader, shadow_map, wrld, fv, light,
                                          kBlockPalette, view_frustum);
        render::draw_water(water_shader, water, fv, light,
                           static_cast<float>(world::kSeaLevel));

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
        postfx.resolve_to_backbuffer(bright_shader, blur_shader, tonemap_shader,
                                     fb_w, fb_h,
                                     /*blur_iter*/ 4,
                                     /*threshold*/ 1.0f,
                                     /*intensity*/ 0.7f,
                                     /*exposure*/  1.0f);

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
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
