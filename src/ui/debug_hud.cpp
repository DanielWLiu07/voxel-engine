#include "ui/debug_hud.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>

namespace ui {

bool DebugHud::init(GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter the cwd with imgui.ini
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) return false;
    // Matches our GLFW window hints: GL 4.1 core.
    if (!ImGui_ImplOpenGL3_Init("#version 410 core")) return false;

    initialized_ = true;
    return true;
}

void DebugHud::shutdown() {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void DebugHud::begin_frame() {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void DebugHud::draw_perf_panel(const PerfFrame& f) {
    if (!initialized_ || !visible_) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);

    if (ImGui::Begin("voxel_engine perf", nullptr,
                     ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("%.1f fps   |   %.2f ms/frame", f.fps, f.frame_ms);
        ImGui::Separator();

        ImGui::Text("chunks drawn : %d / %d", f.chunks_drawn, f.chunks_total);
        if (f.chunks_total > 0) {
            float cull_ratio = static_cast<float>(f.chunks_total) /
                std::max(1, f.chunks_drawn);
            ImGui::Text("frustum cull : %.1fx (skipped %d chunks)",
                        cull_ratio, f.chunks_total - f.chunks_drawn);
        }
        ImGui::Text("triangles    : %zu", f.triangles_drawn);
        ImGui::Text("pending gen  : %d", f.pending_async);

        if (f.initial_load_ms > 0.0 && f.total_chunks > 0) {
            ImGui::Separator();
            double cps = f.total_chunks * 1000.0 / f.initial_load_ms;
            ImGui::Text("startup load : %d chunks in %.0f ms",
                        f.total_chunks, f.initial_load_ms);
            ImGui::Text("throughput   : %.0f chunks/sec (%zu workers)",
                        cps, f.worker_count);
        }

        ImGui::Separator();
        ImGui::TextDisabled("F2: toggle HUD   Tab: release mouse   ESC: quit");
    }
    ImGui::End();
}

void DebugHud::end_frame_and_render() {
    if (!initialized_) return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace ui
