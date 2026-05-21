#pragma once

#include <GLFW/glfw3.h>

namespace core {

// Thin per-frame input state over GLFW. Owns the captured-cursor toggle so
// the camera can do mouselook without snagging the system cursor.
class Input {
public:
    void attach(GLFWwindow* w);
    void begin_frame();

    bool key_down(int key) const;
    bool key_pressed(int key);   // true once on the frame it transitions down

    bool mouse_button_down(int button) const;
    bool mouse_button_pressed(int button);  // edge: down this frame, up last

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

    static constexpr int kKeyMax = GLFW_KEY_LAST + 1;
    bool key_was_down_[kKeyMax]{};

    static constexpr int kMouseButtonMax = GLFW_MOUSE_BUTTON_LAST + 1;
    bool mouse_was_down_[kMouseButtonMax]{};
};

}  // namespace core
