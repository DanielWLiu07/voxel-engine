#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/input.h"
#include "gfx/camera.h"
#include "gfx/mesh.h"
#include "gfx/shader.h"
#include "gfx/texture.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

// Fill a chunk with a sine-bumped terrain surface. Bedrock at y=0..base,
// then grass on top of a sine-warped surface. Good visual test for mesher.
static void fill_test_terrain(world::Chunk& c) {
    using namespace world;
    constexpr int base = 8;
    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            float fx = static_cast<float>(x);
            float fz = static_cast<float>(z);
            float h = std::sin(fx * 0.45f) * 2.0f
                    + std::cos(fz * 0.6f)  * 2.0f
                    + std::sin((fx + fz) * 0.2f) * 1.5f;
            int height = base + static_cast<int>(std::round(h));
            for (int y = 0; y <= height; ++y) {
                BlockId b = BlockId::Stone;
                if (y == height) b = BlockId::Grass;
                else if (y >= height - 2) b = BlockId::Dirt;
                c.set(x, y, z, b);
            }
        }
    }
}

int main(int argc, char** argv) {
    (void)argc;

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

    // Build a single test chunk with sine-bumped terrain and mesh it
    // with the naive mesher. This is the perf baseline that greedy will
    // be compared against in the next commit.
    world::Chunk chunk;
    fill_test_terrain(chunk);
    auto mesh_data = world::build_chunk_mesh_naive(chunk);

    gfx::Mesh chunk_mesh;
    chunk_mesh.upload(mesh_data.vertices, mesh_data.indices);

    std::printf("[mesh:naive] solid=%d  quads=%d  verts=%zu  tris=%zu  build=%.2f ms\n",
                chunk.solid_count(),
                mesh_data.quad_count,
                mesh_data.vertices.size(),
                mesh_data.indices.size() / 3,
                mesh_data.build_ms);

    gfx::FlyCamera cam;
    cam.set_position({8.0f, 16.0f, 28.0f});
    cam.set_yaw_pitch(-110.0f, -25.0f);

    core::Input input;
    input.attach(window);
    input.set_cursor_captured(true);

    std::printf("[input] WASD = move, Space/LCtrl = up/down, mouse = look\n");
    std::printf("[input] Tab = toggle mouse capture, ESC = quit\n");

    double last_time = glfwGetTime();
    double prev_frame_time = glfwGetTime();
    int frame_count = 0;

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = static_cast<float>(now - prev_frame_time);
        prev_frame_time = now;

        input.begin_frame();

        if (input.key_down(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (input.key_pressed(GLFW_KEY_TAB)) {
            input.set_cursor_captured(!input.cursor_captured());
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

            float speed = input.key_down(GLFW_KEY_LEFT_SHIFT) ? 32.0f : 8.0f;
            cam.move_local(local, speed, dt);
        }

        glClearColor(0.50f, 0.70f, 0.92f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;

        glm::mat4 view = cam.view_matrix();
        glm::mat4 proj = cam.proj_matrix(aspect);
        glm::mat4 model(1.0f);

        shader.use();
        shader.set_mat4("u_model", model);
        shader.set_mat4("u_view",  view);
        shader.set_mat4("u_proj",  proj);
        shader.set_vec3("u_light_dir", glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f)));
        shader.set_int("u_albedo", 0);
        tex.bind(0);

        chunk_mesh.draw();

        glfwSwapBuffers(window);
        glfwPollEvents();

        ++frame_count;
        if (now - last_time >= 1.0) {
            char title[192];
            std::snprintf(title, sizeof(title),
                          "voxel_engine  |  %d fps  |  pos %.1f %.1f %.1f  |  tris %zu (naive)",
                          frame_count,
                          cam.position().x, cam.position().y, cam.position().z,
                          mesh_data.indices.size() / 3);
            glfwSetWindowTitle(window, title);
            frame_count = 0;
            last_time = now;
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
