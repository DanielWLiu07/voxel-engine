#pragma once

#include <GLFW/glfw3.h>

namespace core {

class Input {
public:
    void attach(GLFWwindow* w);
    // GLFW delivers scroll through a callback, so the window owner hands
    // it here rather than us polling for it.
    void add_scroll(float dy) { scroll_accum_ += dy; }
    void begin_frame();

    bool key_down(int key) const;

    // Scroll wheel, accumulated since the last begin_frame().
    //
    // Added for the 4D slice rotation, which is the control 4D Miner puts
    // on the wheel: it is a continuous one-dimensional input with no
    // natural key, and binding it to keys would make rotating the cut feel
    // like a different kind of action from what it is.
    float scroll_dy() const { return scroll_dy_; }
    bool key_pressed(int key);   // edge: down this frame, up last

    bool mouse_button_down(int button) const;
    bool mouse_button_pressed(int button);

    float mouse_dx() const { return mouse_dx_; }
    float mouse_dy() const { return mouse_dy_; }

    void set_cursor_captured(bool capture);
    bool cursor_captured() const { return captured_; }

private:
    GLFWwindow* window_ = nullptr;

    double last_mx_ = 0.0;
    double last_my_ = 0.0;
    bool have_last_mouse_ = false;
    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;

    bool captured_ = false;
    float scroll_accum_ = 0.0f;
    float scroll_dy_ = 0.0f;

    static constexpr int kKeyMax = GLFW_KEY_LAST + 1;
    bool key_was_down_[kKeyMax]{};

    static constexpr int kMouseButtonMax = GLFW_MOUSE_BUTTON_LAST + 1;
    bool mouse_was_down_[kMouseButtonMax]{};
};

}  // namespace core
