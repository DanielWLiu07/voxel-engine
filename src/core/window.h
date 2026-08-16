#pragma once

#include <optional>

struct GLFWwindow;

namespace core {

// The GLFW window and its OpenGL 4.1 core context, owned as one thing.
//
// This was sixty lines at the top of main(): init, window hints, context
// creation, the GLAD loader, the framebuffer callback, and a local
// WindowGuard struct whose only job was to undo it all. Three of those
// steps can fail, and the failure paths did not agree with each other -
// the one for a failed GLAD load returned before the guard existed, so it
// leaked the window and never called glfwTerminate(). That is harmless in
// a process that is exiting anyway and it is exactly the kind of thing a
// half-built RAII wrapper is for, so this finishes the wrapper.
//
// Ordering still matters and cannot be enforced from here: every
// GL-owning object (shaders, meshes, FBOs) must be destroyed while the
// context is still current, so the Window has to be declared BEFORE them
// and therefore destructs after them. Holding it in an optional at the
// top of main is what gives it that lifetime.
class Window {
public:
    struct Config {
        int width  = 1280;
        int height = 720;
        const char* title = "voxel_engine";
        // Headless modes (bench, validate, save) create the context but
        // never show a window.
        bool visible = true;
        bool vsync = true;
        int msaa_samples = 2;
    };

    // Returns nullopt after printing the reason. Any partially built state
    // is torn down first, so a caller can simply return on failure.
    static std::optional<Window> create(const Config& config);

    ~Window();
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    GLFWwindow* handle() const { return window_; }
    int gl_version_major() const { return gl_major_; }
    int gl_version_minor() const { return gl_minor_; }

    void set_vsync(bool on);

private:
    Window() = default;

    GLFWwindow* window_ = nullptr;
    int gl_major_ = 0;
    int gl_minor_ = 0;
};

}  // namespace core
