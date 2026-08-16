#include "core/window.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <utility>

namespace core {
namespace {

void glfw_error(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}

void framebuffer_resize(GLFWwindow*, int w, int h) {
    glViewport(0, 0, w, h);
}

}  // namespace

std::optional<Window> Window::create(const Config& config) {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return std::nullopt;
    }

    // 4.1 core is the ceiling on macOS, which froze OpenGL in 2018. Every
    // other platform this builds on has at least that.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, config.msaa_samples);
    if (!config.visible) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(config.width, config.height,
                                          config.title, nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return std::nullopt;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(config.vsync ? 1 : 0);

    const int version = gladLoadGL(glfwGetProcAddress);
    if (version == 0) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        // The old inline version returned here without doing either of
        // these, leaking the window and leaving GLFW initialised.
        glfwDestroyWindow(window);
        glfwTerminate();
        return std::nullopt;
    }

    glfwSetFramebufferSizeCallback(window, framebuffer_resize);
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);

    Window out;
    out.window_ = window;
    out.gl_major_ = GLAD_VERSION_MAJOR(version);
    out.gl_minor_ = GLAD_VERSION_MINOR(version);
    return out;
}

Window::~Window() {
    if (!window_) return;
    glfwDestroyWindow(window_);
    glfwTerminate();
}

Window::Window(Window&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)),
      gl_major_(other.gl_major_),
      gl_minor_(other.gl_minor_) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (window_) {
            glfwDestroyWindow(window_);
            glfwTerminate();
        }
        window_ = std::exchange(other.window_, nullptr);
        gl_major_ = other.gl_major_;
        gl_minor_ = other.gl_minor_;
    }
    return *this;
}

void Window::set_vsync(bool on) { glfwSwapInterval(on ? 1 : 0); }

}  // namespace core
