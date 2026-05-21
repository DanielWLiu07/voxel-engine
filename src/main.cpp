#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/input.h"
#include "core/thread_pool.h"
#include "game/player.h"
#include "gfx/camera.h"
#include "gfx/frustum.h"
#include "gfx/shader.h"
#include "ui/debug_hud.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/terrain_gen.h"
#include "world/world.h"

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

static void glfw_error(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}

static void framebuffer_resize(GLFWwindow*, int w, int h) {
    glViewport(0, 0, w, h);
}

static fs::path find_asset_root(const char* argv0) {
    fs::path start = fs::absolute(argv0).parent_path();
    for (fs::path p = start; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "shaders" / "basic.vert")) return p;
        if (p == p.root_path()) break;
    }
    return fs::current_path();
}

static int run_bench() {
    // Benchmark on real Perlin terrain (the same one the game uses) so
    // the numbers we quote on the resume reflect what the renderer actually
    // sees, not a synthetic best/worst case.
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

    int naive_quads  = last_naive.quad_count;
    int greedy_quads = last_greedy.quad_count;
    size_t naive_tris  = last_naive.indices.size()  / 3;
    size_t greedy_tris = last_greedy.indices.size() / 3;

    std::printf("==== chunk mesher benchmark (%d runs, Perlin terrain chunk 0,0) ====\n", kRuns);
    std::printf("naive : quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                naive_quads, naive_tris, naive_total / kRuns);
    std::printf("greedy: quads=%6d  tris=%6zu  avg build=%6.3f ms\n",
                greedy_quads, greedy_tris, greedy_total / kRuns);
    if (greedy_quads > 0 && greedy_tris > 0) {
        std::printf("ratio : %.1fx fewer quads  |  %.1fx fewer tris\n",
                    static_cast<double>(naive_quads) / greedy_quads,
                    static_cast<double>(naive_tris)  / greedy_tris);
    }
    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--bench") {
            return run_bench();
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
    glfwWindowHint(GLFW_SAMPLES, 4);  // 4x MSAA

    GLFWwindow* window = glfwCreateWindow(1280, 720, "voxel_engine", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    std::printf("GL %d.%d  |  vendor=%s  |  renderer=%s\n",
                GLAD_VERSION_MAJOR(version),
                GLAD_VERSION_MINOR(version),
                glGetString(GL_VENDOR),
                glGetString(GL_RENDERER));

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

    gfx::Shader shader;
    if (!shader.load((root / "shaders" / "basic.vert").string(),
                     (root / "shaders" / "basic.frag").string())) {
        std::fprintf(stderr, "shader load failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    gfx::Shader sky_shader;
    if (!sky_shader.load((root / "shaders" / "sky.vert").string(),
                         (root / "shaders" / "sky.frag").string())) {
        std::fprintf(stderr, "sky shader load failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // Empty VAO required for attributeless fullscreen-triangle draws
    // (core profile won't let you draw without a bound VAO).
    GLuint sky_vao = 0;
    glGenVertexArrays(1, &sky_vao);

    // Block-color palette (BlockId-indexed). Sent to the terrain shader
    // each frame as a uniform array so blocks read as their real materials.
    const glm::vec3 block_palette[8] = {
        {1.00f, 0.00f, 1.00f},  // 0 Air (never seen; magenta = bug flag)
        {0.55f, 0.55f, 0.58f},  // 1 Stone
        {0.50f, 0.34f, 0.20f},  // 2 Dirt
        {0.34f, 0.62f, 0.27f},  // 3 Grass
        {0.88f, 0.80f, 0.55f},  // 4 Sand
        {0.42f, 0.27f, 0.13f},  // 5 Wood
        {0.22f, 0.46f, 0.20f},  // 6 Leaves
        {1.00f, 0.00f, 1.00f},  // 7 (unused)
    };

    // (2r+1)^2 chunks around the origin. radius=12 -> 25x25 = 625 chunks,
    // 400x400 blocks. Async pool generates + meshes off the main thread;
    // main thread uploads VBOs as they finish. Larger world makes frustum
    // culling pay off: most chunks are behind/beside the camera at any
    // given heading.
    constexpr int kRadius = 12;
    const std::size_t worker_count = std::max<std::size_t>(2,
        std::thread::hardware_concurrency() - 1);

    world::TerrainGen terrain(1337);
    world::World wrld;
    core::ThreadPool pool(worker_count);

    const int total_chunks = (2 * kRadius + 1) * (2 * kRadius + 1);
    std::printf("[world] enqueuing %d chunks (radius=%d) onto %zu workers\n",
                total_chunks, kRadius, worker_count);

    auto async_t0 = std::chrono::steady_clock::now();
    wrld.enqueue_grid_async(kRadius, terrain, pool);

    bool initial_load_logged = false;
    double initial_load_ms = 0.0;

    gfx::FlyCamera cam;
    cam.set_position({0.0f, 80.0f, 80.0f});
    cam.set_yaw_pitch(-90.0f, -35.0f);

    core::Input input;
    input.attach(window);
    input.set_cursor_captured(true);

    ui::DebugHud hud;
    if (!hud.init(window)) {
        std::fprintf(stderr, "imgui init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    game::Player player;
    player.set_position({0.0f, 80.0f, 0.0f});
    bool walk_mode = false;

    // Day/night state. time_of_day is in [0, 1) where 0.25 = sunrise,
    // 0.5 = noon, 0.75 = sunset. Starts at midmorning.
    float time_of_day = 0.35f;
    float day_speed   = 1.0f / 240.0f;  // one day per ~4 minutes
    bool  time_paused = false;

    std::printf("[input] WASD = move, Space = jump (walk) / up (fly), LCtrl = down (fly)\n");
    std::printf("[input] LClick = break, RClick = place, Shift = sprint\n");
    std::printf("[input] F = toggle walk/fly, Tab = mouse capture, F2 = HUD, ESC = quit\n");
    std::printf("[input] T = pause time, [/] = step time backwards/forwards\n");

    double last_time = glfwGetTime();
    double prev_frame_time = glfwGetTime();
    int frame_count = 0;
    world::DrawStats last_stats{};
    float smoothed_fps = 0.0f;
    float smoothed_frame_ms = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = static_cast<float>(now - prev_frame_time);
        prev_frame_time = now;

        input.begin_frame();

        // Smooth frame timing for the HUD (exponential moving average).
        float frame_ms = dt * 1000.0f;
        smoothed_frame_ms = smoothed_frame_ms * 0.9f + frame_ms * 0.1f;
        float instant_fps = (dt > 0.0f) ? (1.0f / dt) : 0.0f;
        smoothed_fps = smoothed_fps * 0.9f + instant_fps * 0.1f;

        if (input.key_down(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (input.key_pressed(GLFW_KEY_TAB)) {
            input.set_cursor_captured(!input.cursor_captured());
        }
        if (input.key_pressed(GLFW_KEY_F2)) {
            hud.toggle_visible();
        }
        if (input.key_pressed(GLFW_KEY_T)) time_paused = !time_paused;
        if (input.key_down(GLFW_KEY_RIGHT_BRACKET)) time_of_day += dt * 0.05f;
        if (input.key_down(GLFW_KEY_LEFT_BRACKET))  time_of_day -= dt * 0.05f;
        if (!time_paused) time_of_day += dt * day_speed;
        time_of_day -= std::floor(time_of_day);  // wrap [0, 1)

        // Sun arcs through the sky. At time 0.25 sun is at horizon east,
        // 0.5 noon, 0.75 horizon west, 0/1 midnight (below ground).
        float sun_angle = (time_of_day - 0.25f) * 6.2831853f;
        glm::vec3 sun_dir = glm::normalize(glm::vec3(
            std::cos(sun_angle) * 0.3f + 0.05f,
            std::sin(sun_angle),
            std::cos(sun_angle) * 0.6f));
        float sun_height = sun_dir.y;  // -1..1

        // Color ramps. Daylight is warm-white at noon, deeply orange at
        // horizon, dim blue when below the horizon.
        auto mix3 = [](const glm::vec3& a, const glm::vec3& b, float t) {
            t = glm::clamp(t, 0.0f, 1.0f);
            return a * (1.0f - t) + b * t;
        };
        glm::vec3 sun_color_noon (1.05f, 0.98f, 0.90f);
        glm::vec3 sun_color_dusk (1.20f, 0.55f, 0.25f);
        glm::vec3 sun_color_night(0.05f, 0.07f, 0.15f);
        glm::vec3 sky_top_noon   (0.30f, 0.55f, 0.85f);
        glm::vec3 sky_top_dusk   (0.18f, 0.20f, 0.40f);
        glm::vec3 sky_top_night  (0.02f, 0.02f, 0.06f);
        glm::vec3 sky_horizon_noon (0.78f, 0.86f, 0.93f);
        glm::vec3 sky_horizon_dusk (1.00f, 0.55f, 0.30f);
        glm::vec3 sky_horizon_night(0.05f, 0.06f, 0.10f);
        glm::vec3 ambient_noon (0.32f, 0.34f, 0.40f);
        glm::vec3 ambient_dusk (0.20f, 0.15f, 0.15f);
        glm::vec3 ambient_night(0.05f, 0.06f, 0.10f);

        // Three regimes: night (sun_height < 0), dusk/dawn (0..0.25),
        // daylight (>0.25). Smooth between them.
        float day_t  = glm::clamp((sun_height - 0.05f) / 0.30f, 0.0f, 1.0f);   // 0 at horizon, 1 at high noon
        float night_t = glm::clamp(-sun_height / 0.15f, 0.0f, 1.0f);            // 0 at horizon, 1 well below

        glm::vec3 sun_color  = mix3(mix3(sun_color_night, sun_color_dusk, 1.0f - night_t),
                                    sun_color_noon, day_t);
        glm::vec3 sky_top    = mix3(mix3(sky_top_night, sky_top_dusk, 1.0f - night_t),
                                    sky_top_noon, day_t);
        glm::vec3 sky_horizon= mix3(mix3(sky_horizon_night, sky_horizon_dusk, 1.0f - night_t),
                                    sky_horizon_noon, day_t);
        glm::vec3 ambient    = mix3(mix3(ambient_night, ambient_dusk, 1.0f - night_t),
                                    ambient_noon, day_t);

        // When the sun is below the horizon, point the "light_dir" up so
        // ambient still works correctly (otherwise the sun-down hemisphere
        // would be too dark to see).
        glm::vec3 light_dir = (sun_height > 0.0f)
            ? sun_dir
            : glm::normalize(glm::vec3(sun_dir.x * 0.2f, 0.4f, sun_dir.z * 0.2f));

        // Defer the clipboard copy until after we populate PerfFrame below.
        bool copy_perf_requested = input.key_pressed(GLFW_KEY_C);

        if (input.key_pressed(GLFW_KEY_F)) {
            walk_mode = !walk_mode;
            if (walk_mode) {
                // Drop the player from where the fly camera is.
                player.set_position(cam.position() -
                    glm::vec3(0.0f, game::Player::kEyeHeight, 0.0f));
            }
            std::printf("[mode] %s\n", walk_mode ? "walk" : "fly");
        }

        if (input.cursor_captured()) {
            cam.apply_mouse_delta(input.mouse_dx(), input.mouse_dy(), 0.12f);

            if (walk_mode) {
                // Horizontal movement only; gravity + jump handled by Player.
                glm::vec3 fwd = cam.forward();
                glm::vec3 right = cam.right();
                fwd.y = 0.0f; right.y = 0.0f;
                if (glm::dot(fwd, fwd) > 0.0f) fwd = glm::normalize(fwd);
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

                bool jump = input.key_pressed(GLFW_KEY_SPACE);
                player.update(wrld, wish, jump, dt);
                cam.set_position(player.eye_position());
            } else {
                glm::vec3 local{0.0f};
                if (input.key_down(GLFW_KEY_W)) local.z += 1.0f;
                if (input.key_down(GLFW_KEY_S)) local.z -= 1.0f;
                if (input.key_down(GLFW_KEY_D)) local.x += 1.0f;
                if (input.key_down(GLFW_KEY_A)) local.x -= 1.0f;
                if (input.key_down(GLFW_KEY_SPACE))        local.y += 1.0f;
                if (input.key_down(GLFW_KEY_LEFT_CONTROL)) local.y -= 1.0f;

                float speed = input.key_down(GLFW_KEY_LEFT_SHIFT) ? 60.0f : 16.0f;
                cam.move_local(local, speed, dt);
            }

            // Place / break (left = break, right = place).
            bool break_block = input.mouse_button_pressed(GLFW_MOUSE_BUTTON_LEFT);
            bool place_block = input.mouse_button_pressed(GLFW_MOUSE_BUTTON_RIGHT);
            if (break_block || place_block) {
                auto hit = wrld.raycast(cam.position(), cam.forward(), 8.0f);
                if (hit.hit) {
                    if (break_block) {
                        wrld.set_block(hit.block_x, hit.block_y, hit.block_z,
                                       world::BlockId::Air);
                    } else {
                        int px = hit.block_x + hit.nx;
                        int py = hit.block_y + hit.ny;
                        int pz = hit.block_z + hit.nz;
                        // Don't place a block inside the player.
                        glm::vec3 feet = walk_mode
                            ? player.feet_position()
                            : cam.position() - glm::vec3(0.0f,
                                game::Player::kEyeHeight, 0.0f);
                        bool would_overlap = false;
                        if (walk_mode) {
                            constexpr float hw = game::Player::kWidth * 0.5f;
                            if (px + 1 > feet.x - hw && px < feet.x + hw &&
                                py + 1 > feet.y      && py < feet.y + game::Player::kHeight &&
                                pz + 1 > feet.z - hw && pz < feet.z + hw) {
                                would_overlap = true;
                            }
                        }
                        if (!would_overlap) {
                            wrld.set_block(px, py, pz, world::BlockId::Stone);
                        }
                    }
                }
            }
        }

        // Drain finished chunks from worker pool and upload them. Capped
        // so a big initial batch doesn't stall the frame.
        wrld.drain_finished(16);
        if (!initial_load_logged && wrld.pending_async() == 0) {
            auto async_t1 = std::chrono::steady_clock::now();
            initial_load_ms = std::chrono::duration<double, std::milli>(
                async_t1 - async_t0).count();
            initial_load_logged = true;
            double chunks_per_sec = (initial_load_ms > 0.0)
                ? (total_chunks * 1000.0 / initial_load_ms) : 0.0;
            std::printf("[world] %d chunks loaded in %.1f ms  (%.0f chunks/sec, %zu workers)\n",
                        total_chunks, initial_load_ms, chunks_per_sec, worker_count);
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;

        glm::mat4 view = cam.view_matrix();
        glm::mat4 proj = cam.proj_matrix(aspect);

        gfx::Frustum frustum;
        frustum.from_view_proj(proj * view);

        // Sky pass: fullscreen triangle, depth test off, depth writes off
        // so terrain naturally draws on top.
        {
            glm::mat4 view_no_trans = view;
            view_no_trans[3] = glm::vec4(0, 0, 0, 1);
            glm::mat4 inv_vp = glm::inverse(proj * view_no_trans);

            glDepthMask(GL_FALSE);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);

            sky_shader.use();
            sky_shader.set_mat4("u_inv_view_proj", inv_vp);
            sky_shader.set_vec3("u_sky_top", sky_top);
            sky_shader.set_vec3("u_sky_horizon", sky_horizon);
            sky_shader.set_vec3("u_sun_dir", sun_dir);
            sky_shader.set_vec3("u_sun_color", sun_color);
            glBindVertexArray(sky_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);

            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
        }

        // Fog terminates near the chunk-streaming horizon so chunks
        // dissolve into the sky color rather than vanishing at a sharp
        // far-plane line.
        const float fog_end   = static_cast<float>(kRadius * world::kChunkSizeX) * 0.95f;
        const float fog_start = fog_end * 0.55f;

        shader.use();
        shader.set_mat4("u_view", view);
        shader.set_mat4("u_proj", proj);
        shader.set_vec3("u_light_dir", light_dir);
        shader.set_vec3("u_light_color", sun_color);
        shader.set_vec3("u_ambient_color", ambient);
        shader.set_vec3("u_camera_pos", cam.position());
        shader.set_vec3("u_fog_color", sky_horizon);
        shader.set_float("u_fog_start", fog_start);
        shader.set_float("u_fog_end", fog_end);

        // Palette uniform array. glUniform3fv with count=8 fills u_palette[0..7].
        GLint pal_loc = glGetUniformLocation(shader.id(), "u_palette");
        if (pal_loc >= 0) {
            glUniform3fv(pal_loc, 8, &block_palette[0].x);
        }

        if (input.key_pressed(GLFW_KEY_F1)) {
            std::printf("--- frustum debug ---\n");
            std::printf("cam pos=(%.1f,%.1f,%.1f) yaw=%.1f pitch=%.1f\n",
                        cam.position().x, cam.position().y, cam.position().z,
                        cam.yaw(), cam.pitch());
            glm::vec3 f = cam.forward();
            std::printf("cam fwd=(%.2f,%.2f,%.2f)\n", f.x, f.y, f.z);
            wrld.debug_dump_visibility(frustum);
        }

        last_stats = wrld.draw_visible(frustum, shader);

        // HUD draws on top of the scene, before the swap.
        hud.begin_frame();
        ui::PerfFrame pf;
        pf.frame_ms = smoothed_frame_ms;
        pf.fps = smoothed_fps;
        pf.chunks_total = last_stats.chunks_total;
        pf.chunks_drawn = last_stats.chunks_drawn;
        pf.triangles_drawn = last_stats.triangles_drawn;
        pf.pending_async = wrld.pending_async();
        pf.initial_load_ms = initial_load_ms;
        pf.total_chunks = total_chunks;
        pf.worker_count = worker_count;
        hud.draw_perf_panel(pf);
        if (copy_perf_requested) hud.copy_perf_to_clipboard(pf);
        hud.end_frame_and_render();

        glfwSwapBuffers(window);
        glfwPollEvents();

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
    if (sky_vao) glDeleteVertexArrays(1, &sky_vao);
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
