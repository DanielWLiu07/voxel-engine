#include "core/cli_options.h"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <string_view>

namespace core {

namespace {

// A count flag's argument: a whole number in [lo, hi], or a rejection.
//
// This exists because five flags used std::atoi, which reports failure as
// 0 and cannot distinguish it from a literal "0". `--capture-orbit banana`
// therefore parsed to zero frames, which every downstream `> 0` guard
// reads as "not a capture" - so the engine silently opened the interactive
// window instead of capturing, and a scripted capture hung on a GUI. The
// two flags that already validated (--radius, --threads) used
// strtol-and-range-check; this is that pattern, named once.
//
// Rejects trailing garbage too: strtol alone accepts "6O" as 6, which is
// exactly the typo most likely to be made.
bool parse_count(const char* text, long lo, long hi, const char* flag,
                 int* out, int& exit_code) {
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || v < lo || v > hi) {
        std::fprintf(stderr, "%s expects a whole number between %ld and %ld "
                     "(got \"%s\")\n", flag, lo, hi, text);
        exit_code = EXIT_FAILURE;
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}


// A float flag's argument: a real number in [lo, hi], or a rejection.
//
// The same hole the count flags had, in a place where it hides better.
// std::strtof answers 0 for text it cannot read, and for --time-of-day 0
// is midnight - a legal hour - so `--time-of-day noon` pinned the sun to
// the far side of the planet, wrote a black PNG, and returned success.
bool parse_float(const char* text, float lo, float hi, const char* flag,
                 float* out, int& exit_code) {
    char* end = nullptr;
    errno = 0;
    const float v = std::strtof(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE ||
        !(v >= lo && v <= hi)) {
        std::fprintf(stderr, "%s expects a number between %g and %g "
                     "(got \"%s\")\n", flag, lo, hi, text);
        exit_code = EXIT_FAILURE;
        return false;
    }
    *out = v;
    return true;
}

// A seed: any 32-bit value, but it has to be a number. strtoul reports
// failure as 0 and 0 is a perfectly good seed, so `--seed defualt` used to
// generate an entirely different world without a word about it - the one
// failure mode a reproducibility flag must not have.
bool parse_seed(const char* text, std::uint32_t* out, int& exit_code) {
    char* end = nullptr;
    errno = 0;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE ||
        v > 0xFFFFFFFFul || text[0] == '-') {
        std::fprintf(stderr, "--seed expects a whole number in "
                     "[0, 4294967295] (got \"%s\")\n", text);
        exit_code = EXIT_FAILURE;
        return false;
    }
    *out = static_cast<std::uint32_t>(v);
    return true;
}

// The value that follows a value-taking flag, or nullptr after reporting
// the error. Every one of these used to be guarded by `i + 1 < argc` in
// the flag's own condition, which meant a value flag in last position
// matched nothing at all and fell through to the next test: `voxel_engine
// --radius` streamed at the default radius, in silence, and every number
// it printed was for a run nobody asked for.
const char* value_for(std::string_view flag, int argc, char** argv, int& i,
                      int& exit_code) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "%.*s expects a value\n",
                     static_cast<int>(flag.size()), flag.data());
        exit_code = EXIT_FAILURE;
        return nullptr;
    }
    return argv[++i];
}

// The pose names the frame bench knows. main's pose table falls back to
// "center" for anything it does not recognise, so `--pose caves` used to
// bench the surface and label the row "center" - a plausible-looking
// measurement of the wrong thing.
bool known_pose(std::string_view name) {
    return name == "center" || name == "ground" || name == "high" ||
           name == "cave";
}

}  // namespace

std::optional<CliOptions> parse_cli(int argc, char** argv,
                                    int default_radius, int& exit_code) {
    CliOptions o;
    o.stream_radius = default_radius;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::printf(
                "voxel_engine - desktop voxel engine, C++20 / OpenGL 4.1\n"
                "\n"
                "Usage:\n"
                "  voxel_engine                          launch the gameplay window\n"
                "  voxel_engine --bench                  CPU mesher + cull bench (no GL window)\n"
                "  voxel_engine --bench-frame N          run N vsync-off frames, print BENCH_FRAME\n"
                "  voxel_engine --bench-frame N --pose P bench at named pose (center, ground, high, cave)\n"
                "  voxel_engine --bench-frame N --orbit  bench over a moving camera orbit, not a pose\n"
                "  voxel_engine --seed N                 terrain seed for play/capture/frame bench\n"
                "  voxel_engine --radius N               stream/draw radius in chunks (default 12)\n"
                "  voxel_engine --load DIR               boot from a saved world (RLE snapshot) instead of generating\n"
                "  voxel_engine --save DIR               generate the world, write it to DIR, then exit\n"
                "  voxel_engine --wireframe              start with wireframe terrain (G toggles at runtime)\n"
                "  voxel_engine --threads N              worker pool size, 1-64 (default: cores-1, min 2)\n"
                "  voxel_engine --bench-edit N           N block edits after load, print BENCH_EDIT latency\n"
                "  voxel_engine --validate               load world, verify GPU meshes against voxel data, exit\n"
                "  voxel_engine --verify-edit-persistence  edit, stream away and back, check the edit survived, exit\n"
                "  voxel_engine --bench-frame N --pass-breakdown\n"
                "                                        wall time per render pass (glFinish-bracketed)\n"
                "  voxel_engine --bench-io               save+load the loaded world to /tmp, print BENCH_IO\n"
                "  voxel_engine --screenshot-after N     load world, settle N frames, save PNG, exit\n"
                "  voxel_engine --pose-at x,y,z,yaw,pitch  exact camera placement for shots and\n"
                "  voxel_engine --demo-lights             scatter light sources for a capture\n"
                "  voxel_engine --sky-overdraw            draw the sky first, no depth test\n"
                "                                         (the pre-cloud order; A/B against\n"
                "                                          the default with --pass-breakdown)\n"
                "  voxel_engine --only-chunk cx,cz        draw only that chunk (wireframe\n"
                "                                         captures; pairs with --wireframe\n"
                "                                         and --pose-at, never with --bench)\n"
                "  voxel_engine --naive-mesh              build with the naive mesher\n"
                "                                         (pair with --wireframe to see the\n"
                "                                          quads greedy meshing merges)\n"
                "  voxel_engine --time-of-day 0.28        sun angle for a capture\n"
                "                                         (0.25 sunrise, 0.5 noon, 0.75 sunset)\n"
                "                                        benches (overrides --pose); the water/\n"
                "                                        lake README shot documents an example\n"
                "  voxel_engine --shot-file NAME         filename for --screenshot-after (in ./screenshots)\n"
                "  voxel_engine --orbit-center x,z[,y]   move the orbit/capture circle (default\n"
                "                                        spawn; the lake sits at 288,-400,30)\n"
                "  voxel_engine --capture-orbit N        orbit the scene over N frames, save each\n"
                "                                        to ./capture, exit (README clip source)\n"
                "  voxel_engine --capture-cycle N        fixed pose, one day/night cycle over N\n"
                "                                        frames, save each to ./capture, exit\n"
                "  voxel_engine --no-occlusion           start with occlusion culling disabled\n"
                "  voxel_engine --help                   this text\n"
                "\n"
                "See README.md for the reproducible perf tables and CI gates.\n");
            exit_code = EXIT_SUCCESS;
            return std::nullopt;
        }
        if (arg == "--bench") { o.run_mesher_bench = true; continue; }
        if (arg == "--pass-breakdown") { o.bench_pass_breakdown = true; continue; }
        if (arg == "--orbit") { o.bench_orbit = true; continue; }
        if (arg == "--bench-io") { o.bench_io = true; continue; }
        if (arg == "--wireframe") { o.start_wireframe = true; continue; }
        if (arg == "--validate") { o.validate_mode = true; continue; }
        if (arg == "--verify-edit-persistence") {
            o.verify_edit_persistence = true;
            continue;
        }
        if (arg == "--no-occlusion") { o.no_occlusion = true; continue; }
        if (arg == "--naive-mesh") { o.naive_mesh = true; continue; }
        if (arg == "--sky-overdraw") { o.sky_overdraw = true; continue; }
        if (arg == "--demo-lights") { o.demo_lights = true; continue; }

        if (arg == "--bench-frame") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 1, 1000000, "--bench-frame",
                                   &o.bench_frames, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--screenshot-after") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 1, 100000, "--screenshot-after",
                                   &o.shot_after, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--bench-edit") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 1, 1000000, "--bench-edit",
                                   &o.bench_edit, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        // The two capture counts are bounded well below the frame counts:
        // a capture writes a PNG per frame, so 100k frames is already more
        // disk than the machine has.
        if (arg == "--capture-orbit") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 1, 100000, "--capture-orbit",
                                   &o.orbit_frames, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--capture-cycle") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 1, 100000, "--capture-cycle",
                                   &o.cycle_frames, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        // Below 1 there is no world, and a huge value allocates an
        // enormous chunk grid: 40 chunks each way is 81x81 = 6561 chunks,
        // already well past a comfortable draw distance. parse_count is
        // what makes the bound stick - strtol alone reads "12O" as 12,
        // which is exactly the typo a radius sweep invites.
        if (arg == "--radius") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 1, 40, "--radius",
                                   &o.stream_radius, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--threads") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 1, 64, "--threads",
                                   &o.thread_override, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--seed") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_seed(v, &o.terrain_seed, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--time-of-day") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, 0.0f, 1.0f, "--time-of-day",
                                   &o.time_of_day, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--pose") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            if (!known_pose(v)) {
                std::fprintf(stderr, "--pose expects center, ground, high "
                             "or cave (got \"%s\")\n", v);
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.bench_pose = v;
            continue;
        }
        if (arg == "--shot-file") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            o.shot_file = v;
            continue;
        }
        if (arg == "--load") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            o.load_path = v;
            continue;
        }
        if (arg == "--save") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            o.save_path = v;
            continue;
        }
        // Free-position pose for investigating spots found in screenshots:
        if (arg == "--orbit-center") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            float x = 0, z = 0, look_y = 45.0f;
            if (std::sscanf(v, "%f,%f,%f", &x, &z, &look_y) < 2) {
                std::fprintf(stderr, "--orbit-center expects x,z[,look_y]\n");
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.orbit_center = {x, look_y, z};
            continue;
        }
        // Draws one chunk and discards the rest. Unlike frustum and
        // occlusion culling this throws away geometry the camera can see,
        // deliberately, so it exists for captures only and is never
        // combined with --validate or a bench.
        if (arg == "--only-chunk") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            int cx = 0, cz = 0;
            if (std::sscanf(v, "%d,%d", &cx, &cz) != 2) {
                std::fprintf(stderr, "--only-chunk expects chunk_x,chunk_z\n");
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.have_only_chunk = true;
            o.only_chunk_x = cx;
            o.only_chunk_z = cz;
            continue;
        }
        // --pose-at x,y,z,yaw,pitch (overrides --pose).
        if (arg == "--pose-at") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            float pv[5]{};
            if (std::sscanf(v, "%f,%f,%f,%f,%f",
                            &pv[0], &pv[1], &pv[2], &pv[3], &pv[4]) != 5) {
                std::fprintf(stderr, "--pose-at expects x,y,z,yaw,pitch\n");
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.pose_at = {pv[0], pv[1], pv[2]};
            o.pose_at_yaw = pv[3];
            o.pose_at_pitch = pv[4];
            o.have_pose_at = true;
            continue;
        }

        // Anything left is a typo. It used to fall out of the chain and
        // run the engine on defaults, so `--raduis 8` streamed at 12 and
        // printed a table for a radius nobody asked for. Every branch
        // above continues, so reaching here means nothing matched.
        std::fprintf(stderr, "unrecognised argument \"%s\" (see --help)\n",
                     argv[i]);
        exit_code = EXIT_FAILURE;
        return std::nullopt;
    }

    // The two capture modes are mutually exclusive and need a positive
    // frame count. Rejecting here keeps the render loop's guards simple
    // and avoids the soft-lock a negative atoi would otherwise cause: the
    // input-enable and capture-enable checks would disagree, freezing the
    // camera with no capture and no way out.
    if (o.orbit_frames != 0 && o.cycle_frames != 0) {
        std::fprintf(stderr,
                     "--capture-orbit and --capture-cycle are exclusive\n");
        exit_code = EXIT_FAILURE;
                return std::nullopt;
    }
    // The count, range and sign checks that used to live here are gone
    // because they had become unreachable: every numeric flag now goes
    // through parse_count, which rejects at the point of parse and says
    // which flag and which value. A second bound sitting behind a stricter
    // one reads like a safety net and catches nothing, which is how a
    // check quietly stops being true.
    //
    // --orbit only means anything for the frame bench; label the run so its
    // BENCH_FRAME line is not mistaken for a static pose.
    if (o.bench_orbit && o.bench_frames > 0) o.bench_pose = "orbit";
    return o;
}

}  // namespace core
