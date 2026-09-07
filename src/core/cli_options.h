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
    // --4d: generate the world from the four-dimensional terrain
    // generator, with the player on slice w. The renderer, mesher and
    // culler are unchanged - they only ever see the slice - so this is a
    // change to generation and streaming alone. Off by default, and the
    // 3D engine is exactly what it was.
    // The fourth dimension is ON by default for interactive play, and OFF
    // for every headless measurement path. --3d forces it off, --4d
    // forces it on.
    //
    // The split is deliberate rather than timid. A world is a pure
    // function of (seed, w), so a 4D world at w=0 is not the same world
    // the 3D generator makes - different amplitude, no lakes, no trees.
    // Defaulting the benches and --validate to it would silently move
    // every published figure in the README: gpu_mesh_mb, the greedy
    // ratio the CI gate pins, and the byte-identical BENCH_SUMMARY the
    // invariance check compares. Those numbers describe the 3D engine and
    // have to keep doing so.
    //
    // So: what a player walks around in is four-dimensional. What the
    // repo measures and gates is the same thing it always measured, and
    // asking for a 4D bench is one flag away.
    bool four_d = false;
    bool force_3d = false;
    // --verify-4d: step along w and back, checking the world changes and
    // then returns to exactly what it was. Implies --4d.
    bool verify_4d = false;
    // --trace-input: log every key the engine actually receives, plus the
    // player position and w, to stdout. Exists because "nothing happens"
    // is ambiguous between the engine ignoring input and the engine never
    // being sent any, and those need completely different fixes.
    bool trace_input = false;
    // --auto-w: travel along w by itself, forever, with no input.
    //
    // Exists because "it does not look 4D" and "the keys are not reaching
    // the engine" produce the same picture, and separating them by asking
    // someone to hold a key at the right moment does not work. With this
    // the world morphs on its own: if it does, the fourth dimension is
    // fine and the problem is input; if it does not, it is not.
    bool auto_w = false;
    // --bench-4d: what moving through the fourth dimension costs, for both
    // motions, under real 60 Hz pacing. Implies --4d.
    //
    // The other benches measure a static world. This measures the thing
    // that makes a 4D engine hard: motion along w invalidates geometry
    // rather than just moving the camera through it.
    bool bench_4d = false;
    // --monitor N: open the window on display N instead of wherever GLFW
    // puts it, which on a laptop with external screens attached is the
    // built-in one. --list-monitors prints the indices and exits.
    int  monitor = -1;
    bool list_monitors = false;
    // --capture-tilt N: hold the camera and sweep the slice rotation
    // through a ping-pong, one PNG per frame. Implies --4d.
    int  capture_tilt = 0;
    // --capture-walk N: hold the cut and walk the camera forward, one PNG
    // per frame. Implies --4d.
    //
    // The complement of --capture-tilt, and it exists to answer a
    // question the other captures cannot: on a TILTED cut, does walking
    // move you through the fourth dimension? It does, because the slice's
    // own z axis leans into w - and the only way to show that rather than
    // assert it is a clip where nothing but the camera position changes.
    int  capture_walk = 0;
    int  slice_w = 0;
    // --slice-tilt R: start with the cut rotated R radians, for captures.
    float slice_tilt = 0.0f;
    // --slice-tilt-xw R: the second rotation plane, for captures.
    //
    // Without it the XW plane is unreachable from the command line, so no
    // still or clip could show it and nothing about it was reproducible -
    // the same reason --slice-tilt exists for the first plane.
    float slice_tilt_xw = 0.0f;
    // --warp-walk R: the cut turns as you walk, R radians per block moved.
    //
    // Off by default, and that default is the physically honest one: a
    // hyperplane is fixed, you move within it, so walking reveals more of
    // the same cut rather than changing it. The generator does not even
    // take the player's position.
    //
    // But "everything warps while I walk" is the feel people remember
    // from a 4D game, and it is reachable without lying about the
    // geometry: turn the cut a little for every block travelled and the
    // world genuinely is a different slice by the time you arrive.
    float warp_walk = 0.0f;
    // --slice-prisms: draw blocks as the cross-section of the 4D lattice
    // instead of as cubes. Implies --4d.
    //
    // Off by default and that is deliberate, not caution. A cut turned in
    // ONE plane presents four-sided cells at every angle, so with the
    // wheel alone a cube is not an approximation of the cross-section, it
    // IS the cross-section - and the cube path draws it with the greedy
    // mesher the whole repo is built around. The prism path earns its
    // cost only when both planes are turned at once, which is where a
    // cell becomes a pentagon or a hexagon and a cube starts to be a
    // different shape rather than the same one.
    bool slice_prisms = false;
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
