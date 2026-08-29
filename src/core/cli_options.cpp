#include "core/cli_options.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace core {

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
        if (arg == "--bench") {
            o.run_mesher_bench = true;
            return o;
        }
        if (arg == "--bench-frame" && i + 1 < argc) {
            o.bench_frames = std::atoi(argv[i + 1]);
            ++i;
        }
        if (arg == "--pass-breakdown") o.bench_pass_breakdown = true;
        if (arg == "--orbit") o.bench_orbit = true;
        if (arg == "--seed" && i + 1 < argc) {
            o.terrain_seed = static_cast<std::uint32_t>(
                std::strtoul(argv[i + 1], nullptr, 10));
            ++i;
        }
        if (arg == "--radius" && i + 1 < argc) {
            // Parse as long and range-check before narrowing, so a value
            // past int range is rejected rather than silently truncated
            // into the valid band (atoi would wrap 2^32+12 to 12). The
            // out-of-band sentinel 0 is caught by the bound check below.
            const long r = std::strtol(argv[i + 1], nullptr, 10);
            o.stream_radius = (r >= 1 && r <= 40) ? static_cast<int>(r) : 0;
            ++i;
        }
        if (arg == "--bench-io") o.bench_io = true;
        if (arg == "--pose" && i + 1 < argc) {
            o.bench_pose = argv[i + 1];
            ++i;
        }
        if (arg == "--screenshot-after" && i + 1 < argc) {
            o.shot_after = std::atoi(argv[i + 1]);
            ++i;
        }
        if (arg == "--shot-file" && i + 1 < argc) {
            o.shot_file = argv[i + 1];
            ++i;
        }
        if (arg == "--load" && i + 1 < argc) {
            o.load_path = argv[i + 1];
            ++i;
        }
        if (arg == "--save" && i + 1 < argc) {
            o.save_path = argv[i + 1];
            ++i;
        }
        if (arg == "--wireframe") o.start_wireframe = true;
        if (arg == "--bench-edit" && i + 1 < argc) {
            o.bench_edit = std::atoi(argv[i + 1]);
            ++i;
        }
        if (arg == "--validate") o.validate_mode = true;
        if (arg == "--verify-edit-persistence") o.verify_edit_persistence = true;
        if (arg == "--threads" && i + 1 < argc) {
            // Same strtol-then-range-check pattern as --radius: reject junk
            // and out-of-band values via the 0 sentinel checked below.
            const long t = std::strtol(argv[i + 1], nullptr, 10);
            o.thread_override = (t >= 1 && t <= 64) ? static_cast<int>(t) : -1;
            ++i;
        }
        if (arg == "--capture-orbit" && i + 1 < argc) {
            o.orbit_frames = std::atoi(argv[i + 1]);
            ++i;
        }
        if (arg == "--capture-cycle" && i + 1 < argc) {
            o.cycle_frames = std::atoi(argv[i + 1]);
            ++i;
        }
        if (arg == "--no-occlusion") o.no_occlusion = true;
        // Free-position pose for investigating spots found in screenshots:
        if (arg == "--orbit-center" && i + 1 < argc) {
            float x = 0, z = 0, look_y = 45.0f;
            const int got = std::sscanf(argv[i + 1], "%f,%f,%f", &x, &z, &look_y);
            if (got < 2) {
                std::fprintf(stderr, "--orbit-center expects x,z[,look_y]\n");
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.orbit_center = {x, look_y, z};
            ++i;
            continue;
        }
        // Draws one chunk and discards the rest. Unlike frustum and
        // occlusion culling this throws away geometry the camera can see,
        // deliberately, so it exists for captures only and is never
        // combined with --validate or a bench.
        if (arg == "--only-chunk" && i + 1 < argc) {
            int cx = 0, cz = 0;
            if (std::sscanf(argv[i + 1], "%d,%d", &cx, &cz) != 2) {
                std::fprintf(stderr, "--only-chunk expects chunk_x,chunk_z\n");
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.have_only_chunk = true;
            o.only_chunk_x = cx;
            o.only_chunk_z = cz;
            ++i;
            continue;
        }
        if (arg == "--naive-mesh") { o.naive_mesh = true; continue; }
        if (arg == "--sky-overdraw") { o.sky_overdraw = true; continue; }
        if (arg == "--demo-lights") { o.demo_lights = true; continue; }
        if (arg == "--time-of-day" && i + 1 < argc) {
            const float t = std::strtof(argv[i + 1], nullptr);
            if (!(t >= 0.0f && t <= 1.0f)) {
                std::fprintf(stderr,
                             "--time-of-day expects 0..1 "
                             "(0.25 sunrise, 0.5 noon, 0.75 sunset)\n");
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            o.time_of_day = t;
            ++i;
            continue;
        }
        // --pose-at x,y,z,yaw,pitch (overrides --pose).
        if (arg == "--pose-at" && i + 1 < argc) {
            float v[5]{};
            if (std::sscanf(argv[i + 1], "%f,%f,%f,%f,%f",
                            &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
                o.pose_at = {v[0], v[1], v[2]};
                o.pose_at_yaw = v[3];
                o.pose_at_pitch = v[4];
                o.have_pose_at = true;
            } else {
                std::fprintf(stderr, "--pose-at expects x,y,z,yaw,pitch\n");
                exit_code = EXIT_FAILURE;
                return std::nullopt;
            }
            ++i;
        }
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
    if (o.orbit_frames < 0 || o.cycle_frames < 0) {
        std::fprintf(stderr, "capture frame count must be positive\n");
        exit_code = EXIT_FAILURE;
                return std::nullopt;
    }
    // Bound the frame counts so an absurd value cannot make the sample
    // reserve throw bad_alloc and abort. 10 million frames is already far
    // past any real bench or capture (days of frames).
    constexpr int kMaxFrames = 10'000'000;
    if (o.bench_frames > kMaxFrames || o.orbit_frames > kMaxFrames ||
        o.cycle_frames > kMaxFrames) {
        std::fprintf(stderr, "frame count too large (max %d)\n", kMaxFrames);
        exit_code = EXIT_FAILURE;
                return std::nullopt;
    }
    // --orbit only means anything for the frame bench; label the run so its
    // BENCH_FRAME line is not mistaken for a static pose.
    if (o.bench_orbit && o.bench_frames > 0) o.bench_pose = "orbit";
    // Bound the radius: below 1 there is no world, and a huge value would
    // allocate an enormous chunk grid. 40 chunks each way is 81x81 = 6561
    // chunks, already well past a comfortable draw distance.
    if (o.stream_radius < 1 || o.stream_radius > 40) {
        std::fprintf(stderr, "--radius must be between 1 and 40\n");
        exit_code = EXIT_FAILURE;
                return std::nullopt;
    }
    if (o.thread_override < 0) {
        std::fprintf(stderr, "--threads must be between 1 and 64\n");
        exit_code = EXIT_FAILURE;
                return std::nullopt;
    }
    return o;
}

}  // namespace core
