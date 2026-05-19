#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/input.h"
#include "core/thread_pool.h"
#include "gfx/camera.h"
#include "gfx/frustum.h"
#include "gfx/shader.h"
#include "gfx/texture.h"
#include "ui/debug_hud.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/terrain_gen.h"
#include "world/world.h"

#include <chrono>
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

static std::vector<std::uint8_t> make_checker_rgba(int size, int cells) {
    std::vector<std::uint8_t> px(static_cast<size_t>(size * size * 4));
    int cell = size / cells;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            bool on = ((x / cell) + (y / cell)) % 2 == 0;
            std::uint8_t v = on ? 220 : 60;
            std::uint8_t r = on ? v : static_cast<std::uint8_t>(v + 20);
            std::uint8_t g = on ? static_cast<std::uint8_t>(v - 30) : v;
            std::uint8_t b = on ? static_cast<std::uint8_t>(v - 60)
                                : static_cast<std::uint8_t>(v + 60);
            size_t i = static_cast<size_t>((y * size + x) * 4);
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
        }
    }
    return px;
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

    gfx::Texture2D tex;
    auto checker = make_checker_rgba(256, 8);
    tex.load_from_pixels(checker, 256, 256);

    // (2r+1)^2 chunks around the origin. radius=6 -> 13x13 = 169 chunks,
    // 208x208 blocks. Async pool generates + meshes off the main thread;
    // main thread uploads VBOs as they finish.
    constexpr int kRadius = 6;
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

    std::printf("[input] WASD = move, Space/LCtrl = up/down, Shift = sprint\n");
    std::printf("[input] Tab = toggle mouse capture, F2 = toggle HUD, ESC = quit\n");

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

        if (input.cursor_captured()) {
            cam.apply_mouse_delta(input.mouse_dx(), input.mouse_dy(), 0.12f);

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

        glClearColor(0.50f, 0.70f, 0.92f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;

        glm::mat4 view = cam.view_matrix();
        glm::mat4 proj = cam.proj_matrix(aspect);

        gfx::Frustum frustum;
        frustum.from_view_proj(proj * view);

        shader.use();
        shader.set_mat4("u_view", view);
        shader.set_mat4("u_proj", proj);
        shader.set_vec3("u_light_dir", glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f)));
        shader.set_int("u_albedo", 0);
        tex.bind(0);

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
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
