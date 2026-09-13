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
                "  voxel_engine                          4D world by default; E and Q step along w, wheel rotates the slice\n"
                "  voxel_engine --3d                     the 3D world instead\n"
                "  voxel_engine --trace-input            log every key the engine receives, and w\n"
                "  voxel_engine --auto-w                 travel along w automatically, no input needed\n"
                "  voxel_engine --4d                     force 4D on (benches and captures default to 3D)\n"
                "  voxel_engine --verify-4d              step along w and back, check it returns exactly, exit\n"
                "  voxel_engine --bench-4d               cost of travelling and of rotating the slice, exit\n"
                "  voxel_engine --capture-tilt N         N frames sweeping the 4D slice rotation, exit\n"
                "  voxel_engine --list-monitors          list the displays and their indices, exit\n"
                "  voxel_engine --monitor N              open on display N (default: wherever GLFW puts it)\n"
                "  voxel_engine --slice-w N              start on slice N of the 4D world (implies --4d)\n"
                "  voxel_engine --slice-tilt R           start with the cut turned R radians in ZW\n"
                "  voxel_engine --slice-tilt-xw R        the same in the XW plane\n"
                "  voxel_engine --warp-walk R            the cut turns R rad per block walked\n"
                "  voxel_engine --slice-prisms           draw 4D cross-sections, not cubes\n"
                "  voxel_engine --wind S                 foliage sway strength, 1 default, 0 still\n"
                "  voxel_engine --motes S                fireflies at night / dust by day, 0 off\n"
                "  voxel_engine --weather S              pin weather 0 clear..1 downpour (default: a cycle)\n"
                "  voxel_engine --mist S                 ground mist in the valleys, 0 off\n"
                "  voxel_engine --birds S                flocks circling overhead by day, 0 off\n"
                "  voxel_engine --capture-walk N         N frames walking, cut held, one PNG each\n"
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
        if (arg == "--4d") { o.four_d = true; continue; }
        if (arg == "--wind") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, 0.0f, 20.0f, "--wind",
                                   &o.wind, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--birds") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, 0.0f, 4.0f, "--birds",
                                   &o.birds, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--mist") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, 0.0f, 3.0f, "--mist",
                                   &o.mist, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--weather") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, 0.0f, 1.0f, "--weather",
                                   &o.weather, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--motes") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, 0.0f, 4.0f, "--motes",
                                   &o.motes, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--slice-tilt") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, -3.2f, 3.2f, "--slice-tilt",
                                   &o.slice_tilt, exit_code)) {
                return std::nullopt;
            }
            o.four_d = true;
            continue;
        }
        if (arg == "--capture-walk") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 2, 100000, "--capture-walk",
                                   &o.capture_walk, exit_code)) {
                return std::nullopt;
            }
            o.four_d = true;
            continue;
        }
        if (arg == "--slice-prisms") {
            o.slice_prisms = true;
            o.four_d = true;
            continue;
        }
        if (arg == "--warp-walk") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, 0.0f, 0.05f, "--warp-walk",
                                   &o.warp_walk, exit_code)) {
                return std::nullopt;
            }
            o.four_d = true;
            continue;
        }
        if (arg == "--slice-tilt-xw") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_float(v, -3.2f, 3.2f, "--slice-tilt-xw",
                                   &o.slice_tilt_xw, exit_code)) {
                return std::nullopt;
            }
            o.four_d = true;
            continue;
        }
        if (arg == "--trace-input") { o.trace_input = true; continue; }
        if (arg == "--auto-w") { o.auto_w = true; o.four_d = true; continue; }
        if (arg == "--3d") { o.force_3d = true; continue; }
        if (arg == "--verify-4d") { o.verify_4d = true; o.four_d = true; continue; }
        if (arg == "--bench-4d") { o.bench_4d = true; o.four_d = true; continue; }
        if (arg == "--list-monitors") { o.list_monitors = true; continue; }
        if (arg == "--capture-tilt") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 2, 100000, "--capture-tilt",
                                   &o.capture_tilt, exit_code)) {
                return std::nullopt;
            }
            o.four_d = true;
            continue;
        }
        if (arg == "--monitor") {
            // parse_count, not atoi. atoi reads "banana" as 0 and "1O" as
            // 1, so the run went ahead on the wrong display with no
            // diagnostic - which is the whole class of bug the rest of
            // this parser was written to avoid, reintroduced by a flag
            // that did not go through it. 15 displays is past any real
            // desk and keeps the message finite.
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v || !parse_count(v, 0, 15, "--monitor",
                                   &o.monitor, exit_code)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--slice-w") {
            const char* v = value_for(arg, argc, argv, i, exit_code);
            if (!v) return std::nullopt;
            // Signed, unlike every other count flag, because w runs both
            // ways from the origin - so this cannot use parse_count.
            char* end = nullptr;
            errno = 0;
            const long sv = std::strtol(v, &end, 10);
            if (end == v || *end != '\0' || errno == ERANGE ||
                sv < -1000 || sv > 1000) {
                std::fprintf(stderr, "--slice-w expects a whole number "
                             "between -1000 and 1000 (got \"%s\")\n", v);
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.slice_w = static_cast<int>(sv);
            o.four_d = true;   // asking for a slice implies the 4D world
            continue;
        }
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

    // The capture modes are mutually exclusive and need a positive frame
    // count. Rejecting here keeps the render loop's guards simple and
    // avoids the soft-lock a negative atoi would otherwise cause: the
    // input-enable and capture-enable checks would disagree, freezing the
    // camera with no capture and no way out.
    {
        const int modes = (o.orbit_frames != 0 ? 1 : 0) +
                          (o.cycle_frames != 0 ? 1 : 0) +
                          (o.capture_tilt != 0 ? 1 : 0);
        if (modes > 1) {
            std::fprintf(stderr, "--capture-orbit, --capture-cycle and "
                                 "--capture-tilt are exclusive\n");
            exit_code = EXIT_FAILURE;
            return std::nullopt;
        }
    }
    if (o.force_3d && o.capture_tilt != 0) {
        std::fprintf(stderr, "--3d and --capture-tilt are contradictory\n");
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

    // The fourth dimension defaults on for play and off for measurement.
    // Decided here rather than at the flag, because it depends on which
    // other modes were asked for and those can appear in any order.
    const bool measuring = o.run_mesher_bench || o.bench_frames > 0 ||
                           o.validate_mode || o.verify_edit_persistence ||
                           o.bench_edit > 0 || o.bench_io ||
                           !o.save_path.empty() || !o.load_path.empty() ||
                           o.shot_after > 0 || o.orbit_frames > 0 ||
                           o.cycle_frames > 0;
    if (o.force_3d) {
        o.four_d = false;
    } else if (!measuring) {
        // Interactive play, nothing measured: four dimensions.
        o.four_d = true;
    }
    // A capture or a bench can still ask for 4D explicitly, and --verify-4d
    // already sets four_d itself, so neither is overridden here.
    if (o.force_3d && (o.verify_4d || o.bench_4d)) {
        std::fprintf(stderr, "--3d and %s are contradictory\n",
                     o.verify_4d ? "--verify-4d" : "--bench-4d");
        exit_code = EXIT_FAILURE;
        return std::nullopt;
    }
    return o;
}

}  // namespace core
