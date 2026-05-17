#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/input.h"
#include "gfx/camera.h"
#include "gfx/cube.h"
#include "gfx/mesh.h"
#include "gfx/shader.h"
#include "gfx/texture.h"

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

    gfx::Mesh cube;
    gfx::build_unit_cube(cube);

    gfx::Texture2D tex;
    auto checker = make_checker_rgba(256, 8);
    tex.load_from_pixels(checker, 256, 256);

    gfx::FlyCamera cam;
    cam.set_position({8.0f, 4.0f, 12.0f});
    cam.set_yaw_pitch(-110.0f, -15.0f);

    core::Input input;
    input.attach(window);
    input.set_cursor_captured(true);

    std::printf("[input] WASD = move, Space/LCtrl = up/down, mouse = look\n");
    std::printf("[input] Tab = toggle mouse capture, ESC = quit\n");

    constexpr int kGridX = 12;
    constexpr int kGridZ = 12;

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

            float speed = input.key_down(GLFW_KEY_LEFT_SHIFT) ? 16.0f : 6.0f;
            cam.move_local(local, speed, dt);
        }

        glClearColor(0.50f, 0.70f, 0.92f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;

        glm::mat4 view = cam.view_matrix();
        glm::mat4 proj = cam.proj_matrix(aspect);

        shader.use();
        shader.set_mat4("u_view", view);
        shader.set_mat4("u_proj", proj);
        shader.set_vec3("u_light_dir", glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f)));
        shader.set_int("u_albedo", 0);
        tex.bind(0);

        for (int z = 0; z < kGridZ; ++z) {
            for (int x = 0; x < kGridX; ++x) {
                glm::mat4 model = glm::translate(
                    glm::mat4(1.0f),
                    glm::vec3(static_cast<float>(x), 0.0f, static_cast<float>(z)));
                shader.set_mat4("u_model", model);
                cube.draw();
            }
        }

        glfwSwapBuffers(window);
        glfwPollEvents();

        ++frame_count;
        if (now - last_time >= 1.0) {
            char title[160];
            std::snprintf(title, sizeof(title),
                          "voxel_engine  |  %d fps  |  pos %.1f %.1f %.1f",
                          frame_count,
                          cam.position().x, cam.position().y, cam.position().z);
            glfwSetWindowTitle(window, title);
            frame_count = 0;
            last_time = now;
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
