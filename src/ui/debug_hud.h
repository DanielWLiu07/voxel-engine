#pragma once

#include <cstddef>

struct GLFWwindow;

namespace ui {

struct PerfFrame {
    float frame_ms = 0.0f;       // last frame CPU time
    float fps = 0.0f;            // smoothed
    int   chunks_total = 0;
    int   chunks_drawn = 0;
    std::size_t triangles_drawn = 0;
    int   pending_async = 0;
    double initial_load_ms = 0.0;
    int    total_chunks = 0;
    std::size_t worker_count = 0;
};

class DebugHud {
public:
    bool init(GLFWwindow* window);
    void shutdown();

    // Call at top of each frame (before any rendering). Pushes a new
    // ImGui frame; window flag lets you skip drawing the HUD without
    // breaking ImGui's begin/end pairing.
    void begin_frame();

    // Draws the perf panel using the supplied numbers.
    void draw_perf_panel(const PerfFrame& f);

    // Call after your scene rendering, before glfwSwapBuffers.
    void end_frame_and_render();

    bool visible() const { return visible_; }
    void toggle_visible() { visible_ = !visible_; }

    // Copy the current perf snapshot to the system clipboard as a
    // markdown-formatted block, suitable for pasting into a commit
    // message or resume draft.
    void copy_perf_to_clipboard(const PerfFrame& f) const;

private:
    bool initialized_ = false;
    bool visible_ = true;
};

}  // namespace ui
