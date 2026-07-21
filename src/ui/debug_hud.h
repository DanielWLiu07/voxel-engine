#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct GLFWwindow;

namespace ui {

struct PerfFrame {
    float frame_ms = 0.0f;
    float fps = 0.0f;
    int   chunks_total = 0;
    int   chunks_drawn = 0;
    int   sections_drawn = 0;
    int   sections_occluded = 0;   // frustum-visible but skipped by the BFS
    bool  occlusion_enabled = false;
    const char* place_block_name = nullptr;
    int   ai_texture_tiles = 0;   // >0 -> show the AI-art credit line
    std::size_t triangles_drawn = 0;
    std::size_t gpu_bytes = 0;   // resident vertex + index buffer bytes
    int   pending_async = 0;
    // Block-edit remesh latency (full synchronous set_block: greedy remesh
    // + re-bucket + GL upload + visibility). Row hidden until edit_count > 0.
    std::uint64_t edit_count = 0;
    double edit_last_ms = 0.0;
    double edit_avg_ms = 0.0;
    double edit_max_ms = 0.0;
    double initial_load_ms = 0.0;
    int    total_chunks = 0;
    std::size_t worker_count = 0;
    int   streamed_in = 0;
    int   streamed_out = 0;
};

class DebugHud {
public:
    bool init(GLFWwindow* window);
    void shutdown();

    void begin_frame();
    void draw_perf_panel(const PerfFrame& f);
    void end_frame_and_render();

    bool visible() const { return visible_; }
    void toggle_visible() { visible_ = !visible_; }

    void copy_perf_to_clipboard(const PerfFrame& f) const;

private:
    // Rolling frame-time history for the perf-panel graph: a fixed ring the
    // panel pushes each frame, so the plot shows the last few seconds of
    // frame times (the shape of a stutter the average and stddev only hint at).
    static constexpr int kFrameHistory = 120;
    std::array<float, kFrameHistory> frame_ms_history_{};
    int frame_history_head_ = 0;

    bool initialized_ = false;
    bool visible_ = true;
};

}  // namespace ui
