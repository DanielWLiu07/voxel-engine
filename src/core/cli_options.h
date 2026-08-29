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
    // --only-chunk x,z: draw one chunk and nothing else. A capture aid for
    // the wireframe shot, where 200 drawn chunks of merged rectangles is
    // an unreadable thicket. Absent by default.
    bool have_only_chunk = false;
    int  only_chunk_x = 0;
    int  only_chunk_z = 0;

    std::uint32_t terrain_seed = 1337;
    int stream_radius = 12;  // overwritten with the caller's default

    int shot_after = 0;
    std::string shot_file;
    std::string load_path;
    std::string save_path;
    bool start_wireframe = false;
    // --naive-mesh: build the world with the one-quad-per-face baseline
    // instead of the greedy mesher. A rendering aid, not a mode anyone
    // should play in - it exists so the greedy win can be looked at in a
    // wireframe rather than only quoted as a ratio.
    bool naive_mesh = false;
    // Draw the sky first with the depth test off, the way it was drawn
    // before the procedural clouds landed. Kept as an A/B so the cost of
    // shading sky under the terrain stays measurable from the repo.
    bool sky_overdraw = false;
    // --demo-lights: scatter emissive blocks around the camera once the
    // world settles. A capture aid, like --time-of-day: block light is
    // only visible where something emits, and terrain generates none, so
    // without this there is no way to photograph the feature.
    bool demo_lights = false;
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

    // Sun position, as a fraction of a day: 0 midnight, 0.25 sunrise,
    // 0.5 noon, 0.75 sunset. Negative means "leave it at the engine's
    // default", which is what interactive runs want.
    //
    // Exposed because a still is only as good as its light. The default
    // sits mid-morning with the sun high, which is the one angle that
    // hides what three cascades of shadow mapping are doing; a low sun
    // rakes shadows across the terrain and shows it. Captures should be
    // able to choose the hour without editing the source.
    float time_of_day = -1.0f;
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
