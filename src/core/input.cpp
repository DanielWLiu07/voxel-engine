#include "core/input.h"

namespace core {

void Input::attach(GLFWwindow* w) {
    window_ = w;
    have_last_mouse_ = false;
    mouse_dx_ = mouse_dy_ = 0.0f;
    captured_ = false;
    for (auto& v : key_was_down_) v = false;
}

void Input::begin_frame() {
    if (!window_) return;

    double mx, my;
    glfwGetCursorPos(window_, &mx, &my);
    if (have_last_mouse_) {
        mouse_dx_ = static_cast<float>(mx - last_mx_);
        mouse_dy_ = static_cast<float>(my - last_my_);
    } else {
        mouse_dx_ = mouse_dy_ = 0.0f;
        have_last_mouse_ = true;
    }
    last_mx_ = mx;
    last_my_ = my;
}

bool Input::key_down(int key) const {
    if (!window_ || key < 0 || key >= kKeyMax) return false;
    return glfwGetKey(window_, key) == GLFW_PRESS;
}

bool Input::key_pressed(int key) {
    if (!window_ || key < 0 || key >= kKeyMax) return false;
    bool down = glfwGetKey(window_, key) == GLFW_PRESS;
    bool transitioned = down && !key_was_down_[key];
    key_was_down_[key] = down;
    return transitioned;
}

void Input::set_cursor_captured(bool capture) {
    if (!window_) return;
    captured_ = capture;
    glfwSetInputMode(window_, GLFW_CURSOR,
                     capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Reset deltas so the next frame doesn't see a big jump.
    have_last_mouse_ = false;
}

}  // namespace core
