#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

namespace core {

// Everything the command line can set, in one place.
//
// This used to be two hundred and fifty lines at the top of main(): twenty
// locals, the parse loop, and the range checks, all ahead of the first line
// of engine code. Parsing arguments is not what main is for, and a reader
// looking for how the engine starts had to scroll past all of it.
struct CliOptions {
    // --bench: the CPU mesher + cull benchmark, which needs no GL window.
    // Reported as a flag rather than run here, because running it is the
    // caller's job and this component knows nothing about meshing.
    bool run_mesher_bench = false;

    int bench_frames = 0;
    bool bench_pass_breakdown = false;
    bool bench_io = false;
    bool bench_orbit = false;
    std::string bench_pose = "center";
    glm::vec3 orbit_center{-10.0f, 45.0f, -10.0f};

    std::uint32_t terrain_seed = 1337;
    int stream_radius = 12;  // overwritten with the caller's default

    int shot_after = 0;
    std::string shot_file;
    std::string load_path;
    std::string save_path;
    bool start_wireframe = false;
    int bench_edit = 0;
    bool validate_mode = false;
    bool verify_edit_persistence = false;
    int thread_override = 0;
    int orbit_frames = 0;
    int cycle_frames = 0;
    bool no_occlusion = false;

    glm::vec3 pose_at{};
    float pose_at_yaw = 0.0f;
    float pose_at_pitch = 0.0f;
    bool have_pose_at = false;
};

// Parses argv and range-checks it. Returns nullopt when the program should
// stop immediately, with `exit_code` set to what main should return: that
// covers --help (success) as well as a bad flag (failure), so the caller
// does not have to tell those apart.
//
// `default_radius` is passed in rather than baked in because the engine's
// default stream radius belongs to the engine, not to argument parsing.
std::optional<CliOptions> parse_cli(int argc, char** argv,
                                    int default_radius, int& exit_code);

}  // namespace core
