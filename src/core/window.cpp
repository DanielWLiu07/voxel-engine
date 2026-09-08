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

    if (config.list_monitors) {
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        std::printf("%d display(s):\n", count);
        for (int i = 0; i < count; ++i) {
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
            std::printf("  --monitor %d  %-24s %dx%d at (%d, %d)%s\n", i,
                        glfwGetMonitorName(monitors[i]), mw, mh, mx, my,
                        monitors[i] == glfwGetPrimaryMonitor()
                            ? "  [primary]" : "");
        }
        glfwTerminate();
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
    // Take keyboard focus when shown.
    //
    // Launched from a terminal on macOS the window appears but focus stays
    // with whatever was frontmost, so every keypress goes somewhere else
    // and the engine looks completely unresponsive - it renders, the HUD
    // updates, and nothing you press does anything. That is indis-
    // tinguishable from a broken input path, and it cost several rounds of
    // debugging the wrong layer.
    //
    // Only for a visible window: a headless bench or capture must never
    // steal focus from whatever the user is actually doing.
    if (config.visible) glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(config.width, config.height,
                                          config.title, nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return std::nullopt;
    }
    glfwMakeContextCurrent(window);
    // Place the window on the requested display before showing it, so it
    // never appears on the wrong screen and jumps.
    if (config.visible && config.monitor >= 0) {
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        if (monitors && config.monitor < count) {
            GLFWmonitor* m = monitors[config.monitor];
            // The WORK area, not the monitor bounds: on macOS the menu bar
            // and Dock are outside it, and centring on the raw bounds puts
            // the title bar under the menu bar on the primary display.
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(m, &mx, &my, &mw, &mh);
            int ww = 0, wh = 0;
            glfwGetWindowSize(window, &ww, &wh);
            glfwSetWindowPos(window, mx + (mw - ww) / 2, my + (mh - wh) / 2);
        } else {
            std::fprintf(stderr,
                         "[window] no display %d (%d attached), using the "
                         "default\n", config.monitor, count);
        }
    }
    // The hint covers the normal case; this covers being launched from a
    // background shell, where the process itself is not frontmost and the
    // hint alone does not raise it.
    if (config.visible) {
        glfwShowWindow(window);
        glfwFocusWindow(window);
    }
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
