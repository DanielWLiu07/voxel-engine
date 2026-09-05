// Unit tests for the command-line parser.
//
// cli_options.cpp is 250-odd lines of pure string -> struct with no GL in
// it, and until this file existed it had no test at all. It is also where
// this project's defects keep turning up, and they share one shape: a bad
// argument does not stop the run, it changes it. `--capture-orbit banana`
// opened the interactive window; `--seed defualt` generated a different
// world; `--time-of-day noon` wrote a black PNG; `--raduis 8` measured
// radius 12 and printed a table saying so.
//
// So the tests here are mostly one invariant applied flag by flag: an
// argument the parser cannot honour has to fail, name itself, and never
// fall through to a default. Run via ctest:
//   cmake --build build -j
//   ctest --test-dir build --output-on-failure

#include "core/cli_options.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, label) do {                                            \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
        std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, label);      \
        ++g_failures;                                                       \
    }                                                                       \
} while (0)

// The engine's own default, passed in rather than baked into the parser.
// Deliberately not 12 (the shipped value) so a test that reads the default
// back cannot pass by coincidence.
constexpr int kTestDefaultRadius = 7;

// What one parse produced: the options (or their absence), the exit code
// the caller would return, and whatever the parser wrote to stderr.
struct ParseResult {
    std::optional<core::CliOptions> opts;
    int exit_code = -999;
    std::string diagnostics;
};

// Runs parse_cli with stderr and stdout diverted to a pipe-backed temp
// file, so a rejection's message can be asserted on instead of scrolling
// past. The parser writes --help to stdout and errors to stderr; both are
// captured so a passing run of this file stays quiet.
ParseResult parse(std::vector<const char*> args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("voxel_engine"));
    for (const char* a : args) argv.push_back(const_cast<char*>(a));

    std::fflush(stderr);
    std::fflush(stdout);
    const int saved_err = ::dup(fileno(stderr));
    const int saved_out = ::dup(fileno(stdout));
    FILE* sink = std::tmpfile();
    ::dup2(fileno(sink), fileno(stderr));
    ::dup2(fileno(sink), fileno(stdout));

    ParseResult r;
    r.opts = core::parse_cli(static_cast<int>(argv.size()), argv.data(),
                             kTestDefaultRadius, r.exit_code);

    std::fflush(stderr);
    std::fflush(stdout);
    ::dup2(saved_err, fileno(stderr));
    ::dup2(saved_out, fileno(stdout));
    ::close(saved_err);
    ::close(saved_out);

    std::rewind(sink);
    char buf[1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, sink)) > 0) r.diagnostics.append(buf, n);
    std::fclose(sink);
    return r;
}

bool mentions(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// A rejection is only useful if it stops the run AND says what was wrong,
// so both halves are one predicate. `blamed` is the flag or the value the
// message has to name for a reader to fix the command without guessing.
bool rejected_naming(const ParseResult& r, const char* blamed) {
    return !r.opts.has_value() && r.exit_code == EXIT_FAILURE &&
           mentions(r.diagnostics, blamed);
}

// ----- the value flags, as one table --------------------------------------

// Every flag that consumes the next argument, with a value that is valid
// for it. Driving the missing-value and trailing-garbage tests off one
// table is the point: the defect these cover was never in one flag, it was
// in the shape the whole chain was written in, so a new flag written the
// old way has to fail here rather than be noticed by eye.
struct ValueFlag {
    const char* flag;
    const char* good;
    bool        numeric;  // takes a plain number, so garbage is checkable
};

constexpr ValueFlag kValueFlags[] = {
    {"--bench-frame",       "10",              true},
    {"--screenshot-after",  "60",              true},
    {"--bench-edit",        "5",               true},
    {"--capture-orbit",     "90",              true},
    {"--capture-cycle",     "90",              true},
    {"--radius",            "8",               true},
    {"--threads",           "4",               true},
    {"--seed",              "1337",            true},
    {"--time-of-day",       "0.75",            true},
    {"--pose",              "cave",            false},
    {"--shot-file",         "shot.png",        false},
    {"--load",              "saves/w",         false},
    {"--save",              "saves/w",         false},
    {"--orbit-center",      "288,-400,30",     false},
    {"--only-chunk",        "0,0",             false},
    {"--pose-at",           "1,2,3,4,5",       false},
};

void test_every_value_flag_accepts_its_own_good_value() {
    for (const auto& f : kValueFlags) {
        const auto r = parse({f.flag, f.good});
        EXPECT(r.opts.has_value(), f.flag);
        if (!r.opts.has_value()) {
            std::printf("    (%s %s was rejected: %s)\n", f.flag, f.good,
                        r.diagnostics.c_str());
        }
    }
}

void test_no_value_flag_survives_a_missing_value() {
    // The flag in last position. Each of these used to fail its own
    // `i + 1 < argc` guard, match nothing, and leave the option at its
    // default - so the run went ahead with a silently different setting.
    for (const auto& f : kValueFlags) {
        const auto r = parse({f.flag});
        EXPECT(rejected_naming(r, f.flag), f.flag);
    }
}

void test_no_numeric_flag_accepts_a_word() {
    for (const auto& f : kValueFlags) {
        if (!f.numeric) continue;
        const auto r = parse({f.flag, "banana"});
        EXPECT(rejected_naming(r, f.flag), f.flag);
    }
}

void test_no_numeric_flag_accepts_trailing_garbage() {
    // "12O" - capital O for zero - is the typo a radius sweep invites, and
    // strtol without an end check reads it as 12 and moves on.
    for (const auto& f : kValueFlags) {
        if (!f.numeric) continue;
        std::string bad = std::string(f.good) + "O";
        const auto r = parse({f.flag, bad.c_str()});
        EXPECT(rejected_naming(r, f.flag), f.flag);
    }
}

// ----- the two values that garbage used to imitate -------------------------

void test_a_bad_seed_is_not_seed_zero() {
    // strtoul answers 0 for text it cannot read, and 0 is a legal seed, so
    // the wrong world and the requested world were indistinguishable.
    const auto good = parse({"--seed", "0"});
    EXPECT(good.opts.has_value() && good.opts->terrain_seed == 0u,
           "--seed 0 is a real seed and is honoured");
    const auto bad = parse({"--seed", "defualt"});
    EXPECT(rejected_naming(bad, "--seed"), "--seed defualt is refused");
    EXPECT(mentions(bad.diagnostics, "defualt"),
           "the seed rejection quotes what it could not read");
}

void test_a_bad_hour_is_not_midnight() {
    // Same shape one flag over: strtof answers 0, and 0 is midnight, so a
    // typo produced a black capture and exit status 0.
    const auto good = parse({"--time-of-day", "0"});
    EXPECT(good.opts.has_value() && good.opts->time_of_day == 0.0f,
           "--time-of-day 0 is midnight and is honoured");
    const auto bad = parse({"--time-of-day", "noon"});
    EXPECT(rejected_naming(bad, "--time-of-day"), "--time-of-day noon is refused");
    // Midnight and "not asked for" must stay distinguishable: the engine
    // reads a negative time_of_day as "keep the interactive default".
    EXPECT(parse({}).opts->time_of_day < 0.0f,
           "an unset hour is negative, not 0");
}

void test_the_hour_is_bounded_to_one_day() {
    EXPECT(parse({"--time-of-day", "0"}).opts.has_value(), "0 accepted");
    EXPECT(parse({"--time-of-day", "1"}).opts.has_value(), "1 accepted");
    EXPECT(rejected_naming(parse({"--time-of-day", "1.5"}), "--time-of-day"),
           "1.5 refused");
    EXPECT(rejected_naming(parse({"--time-of-day", "-0.1"}), "--time-of-day"),
           "-0.1 refused");
}

// ----- bounds, as edges rather than as constants ---------------------------

void test_numeric_bounds_reject_just_outside_and_accept_just_inside() {
    struct Band { const char* flag; const char* lo; const char* below;
                  const char* hi; const char* above; };
    constexpr Band kBands[] = {
        {"--radius",  "1", "0", "40", "41"},
        {"--threads", "1", "0", "64", "65"},
    };
    for (const auto& b : kBands) {
        EXPECT(parse({b.flag, b.lo}).opts.has_value(), b.flag);
        EXPECT(parse({b.flag, b.hi}).opts.has_value(), b.flag);
        EXPECT(rejected_naming(parse({b.flag, b.below}), b.flag), b.flag);
        EXPECT(rejected_naming(parse({b.flag, b.above}), b.flag), b.flag);
    }
    // A value past int range must not wrap into the band: 2^32 + 12 is 12
    // in 32 bits, and reading it as a valid radius is the failure the
    // strtol-and-range-check pattern exists to prevent.
    EXPECT(rejected_naming(parse({"--radius", "4294967308"}), "--radius"),
           "a radius past int range is refused, not wrapped");
}

// ----- the parser's structure, not its individual flags --------------------

void test_the_engines_default_radius_flows_through_untouched() {
    // The default belongs to the engine, not to argument parsing; the
    // parser's job is to pass it along when nobody overrides it.
    EXPECT(parse({}).opts->stream_radius == kTestDefaultRadius,
           "no --radius leaves the caller's default in place");
    EXPECT(parse({"--radius", "9"}).opts->stream_radius == 9,
           "--radius overrides the caller's default");
}

void test_an_unrecognised_argument_stops_the_run() {
    const auto typo = parse({"--raduis", "8"});
    EXPECT(rejected_naming(typo, "--raduis"),
           "a misspelled flag is refused, not ignored");
    // The spelled-right neighbour still works, so the check is on the name
    // and not on anything about the surrounding command.
    EXPECT(parse({"--radius", "8"}).opts.has_value(), "--radius 8 still parses");
    EXPECT(rejected_naming(parse({"world.sav"}), "world.sav"),
           "a stray positional argument is refused too");
}

void test_flag_order_does_not_change_the_run() {
    // --bench used to return from the parse loop the moment it matched, so
    // everything after it was dropped: `--bench --seed 7` and `--seed 7
    // --bench` were two different runs with one command line between them.
    const auto a = parse({"--bench", "--seed", "7"});
    const auto b = parse({"--seed", "7", "--bench"});
    EXPECT(a.opts.has_value() && b.opts.has_value(), "both orders parse");
    EXPECT(a.opts->run_mesher_bench && b.opts->run_mesher_bench,
           "both orders select the bench");
    EXPECT(a.opts->terrain_seed == 7u && b.opts->terrain_seed == 7u,
           "both orders carry the seed");
    EXPECT(a.opts->terrain_seed == b.opts->terrain_seed,
           "order is not a setting");
    // And a typo after --bench is still a typo.
    EXPECT(rejected_naming(parse({"--bench", "--raduis", "8"}), "--raduis"),
           "--bench does not stop the rest of the line being checked");
}

void test_only_the_poses_the_bench_knows_are_accepted() {
    // main's pose table falls back to "center", so an unknown name used to
    // produce a real measurement of the wrong camera.
    for (const char* p : {"center", "ground", "high", "cave"}) {
        const auto r = parse({"--pose", p});
        EXPECT(r.opts.has_value() && r.opts->bench_pose == p, p);
    }
    EXPECT(rejected_naming(parse({"--pose", "caves"}), "--pose"),
           "a near-miss pose name is refused");
}

void test_orbit_relabels_the_pose_only_when_there_is_a_bench_to_label() {
    const auto benched = parse({"--bench-frame", "30", "--orbit"});
    EXPECT(benched.opts->bench_pose == "orbit",
           "an orbit frame bench reports pose=orbit");
    const auto not_benched = parse({"--orbit"});
    EXPECT(not_benched.opts->bench_pose == "center",
           "--orbit with no frame bench leaves the pose name alone");
}

void test_the_two_capture_modes_stay_exclusive() {
    const auto both = parse({"--capture-orbit", "30", "--capture-cycle", "30"});
    EXPECT(rejected_naming(both, "exclusive"), "orbit + cycle is refused");
    EXPECT(parse({"--capture-orbit", "30"}).opts->orbit_frames == 30,
           "orbit alone is fine");
    EXPECT(parse({"--capture-cycle", "30"}).opts->cycle_frames == 30,
           "cycle alone is fine");
}

void test_help_stops_the_program_successfully() {
    for (const char* h : {"--help", "-h"}) {
        const auto r = parse({h});
        EXPECT(!r.opts.has_value(), "--help returns nothing to run");
        EXPECT(r.exit_code == EXIT_SUCCESS, "--help exits successfully");
        EXPECT(mentions(r.diagnostics, "voxel_engine --bench"),
               "--help prints the usage text");
    }
}

// Every flag this test file drives has to be one --help lists, or the
// help text and the parser have drifted apart - which is the same rot the
// README flag check guards from the other side.
void test_help_lists_every_flag_these_tests_use() {
    const std::string help = parse({"--help"}).diagnostics;
    for (const auto& f : kValueFlags) {
        EXPECT(mentions(help, f.flag), f.flag);
    }
    for (const char* f : {"--bench", "--validate", "--wireframe", "--orbit",
                          "--no-occlusion", "--naive-mesh", "--demo-lights",
                          "--sky-overdraw", "--pass-breakdown", "--bench-io",
                          "--verify-edit-persistence"}) {
        EXPECT(mentions(help, f), f);
    }
}

}  // namespace

int main() {
    std::printf("cli_tests: running...\n\n");
    test_every_value_flag_accepts_its_own_good_value();
    test_no_value_flag_survives_a_missing_value();
    test_no_numeric_flag_accepts_a_word();
    test_no_numeric_flag_accepts_trailing_garbage();
    test_a_bad_seed_is_not_seed_zero();
    test_a_bad_hour_is_not_midnight();
    test_the_hour_is_bounded_to_one_day();
    test_numeric_bounds_reject_just_outside_and_accept_just_inside();
    test_the_engines_default_radius_flows_through_untouched();
    test_an_unrecognised_argument_stops_the_run();
    test_flag_order_does_not_change_the_run();
    test_only_the_poses_the_bench_knows_are_accepted();
    test_orbit_relabels_the_pose_only_when_there_is_a_bench_to_label();
    test_the_two_capture_modes_stay_exclusive();
    test_help_stops_the_program_successfully();
    test_help_lists_every_flag_these_tests_use();

    std::printf("\ncli_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
