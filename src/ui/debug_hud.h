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
    // Evicted preserve-marked chunks held as RLE bytes (the write-back
    // stash) and their total size, so stash growth is visible live.
    std::size_t stash_chunks = 0;
    std::size_t stash_bytes  = 0;
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
    // The fourth axis. Row hidden entirely in the 3D engine.
    bool  four_d = false;
    float slice_w = 0.0f;    // where the player is along w
    float meshed_w = 0.0f;   // where the geometry is; lags while moving
    // The 3D slice's orientation within 4D, one angle per rotation
    // plane. Both are shown because one alone does not say where you are
    // looking: a player who has turned only in XW would read theta as 0
    // and conclude the wheel had done nothing.
    float slice_theta = 0.0f;  // ZW plane: the wheel, and vertical mouse
    float slice_phi   = 0.0f;  // XW plane: horizontal mouse
    // Whether blocks are drawn as their 4D cross-section (P toggles it).
    // Worth a line of its own because at a single-plane tilt the two
    // modes are IDENTICAL - a cut turned in one plane presents four-sided
    // cells at every angle - so a player who cannot see the mode has no
    // way to tell the feature is on.
    bool  prisms      = false;
    // Cells the tiling produced for the last chunk built, against the 256
    // voxel columns a flat cut would give. 1.0x flat, rising with tilt.
    float prism_cells_per_column = 0.0f;
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

    // True when the pointer is over a HUD panel, so the caller can leave
    // the mouse to ImGui instead of also acting on it.
    //
    // ImGui's GLFW backend chains whatever scroll callback was installed
    // before it rather than replacing it, so a wheel event over the HUD
    // reaches both: the panel scrolls AND the world rotates. Anything
    // that reads the wheel has to ask first.
    bool wants_mouse() const;

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
