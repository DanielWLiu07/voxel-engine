// The wheel's angle debt: does the cut turn by exactly what was asked,
// and never by a visible jump in one frame?
//
// Both halves matter and they pull against each other. Bounding the
// per-frame step is the whole point of the class, and the cheap way to
// bound it - stop when the remainder is small - loses the tail, so the
// player scrolls a notch and the world turns by slightly less than one.
// Over a long session that drifts, and it would break the byte-identical
// rotation round-trip --verify-4d gates, which needs +d then -d to land
// exactly where it started.

#include "core/slice_ease.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

int checks = 0, failures = 0;

void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL: %s\n", what); }
}

constexpr float kNotch = 0.003f;   // main.cpp's scroll scale
constexpr float kDt    = 1.0f / 60.0f;

// Drain to settled, returning the total turned and the largest single step.
struct Drain { float total; float peak; int frames; };
Drain drain(core::SliceEase& e, float dt = kDt, int limit = 100000) {
    Drain d{0.0f, 0.0f, 0};
    while (!e.settled() && d.frames < limit) {
        const float s = e.step(dt);
        d.total += s;
        if (std::fabs(s) > std::fabs(d.peak)) d.peak = s;
        ++d.frames;
    }
    return d;
}

void test_pays_exactly_what_was_asked() {
    for (int notches : {1, 3, 8, 24, 84}) {
        core::SliceEase e;
        const float want = static_cast<float>(notches) * kNotch;
        e.request(want);
        const Drain d = drain(e);
        expect(e.settled(), "the debt reaches settled");
        expect(std::fabs(e.outstanding()) == 0.0f, "and lands on exactly zero");
        // Summing floats loses a few ulps; a third of a notch would be a
        // visible error and is what a lost tail would cost.
        expect(std::fabs(d.total - want) < 1e-6f,
               "the angle turned is the angle asked for");
    }
}

void test_no_frame_jumps() {
    // A trackpad flick: far more than a frame's worth, all at once.
    core::SliceEase e;
    e.request(84.0f * kNotch);
    const Drain d = drain(e);
    const float cap = core::SliceEase::kMaxRadPerSec * kDt;
    expect(std::fabs(d.peak) <= cap * 1.001f,
           "no single frame exceeds the angular-velocity ceiling");
    expect(std::fabs(d.peak) < kNotch,
           "and so no frame turns as far as one whole notch");
}

void test_starts_immediately() {
    // Easing must not read as input lag: the first frame has to move.
    core::SliceEase e;
    e.request(kNotch);
    const float first = e.step(kDt);
    expect(first > kNotch * 0.15f,
           "the first frame turns a useful fraction of the notch");
    expect(first < kNotch,
           "but not the whole thing, which is what it replaced");
}

void test_opposite_requests_cancel() {
    // Scrolling back before the first turn has arrived must not leave a
    // residue for the cut to keep turning through.
    core::SliceEase e;
    e.request(5.0f * kNotch);
    e.step(kDt);
    e.request(-5.0f * kNotch);
    const Drain d = drain(e);
    const float turned = d.total;   // after the one step already taken
    expect(e.settled(), "cancelling settles");
    // The net of everything requested is zero, so everything turned after
    // the first step has to undo it.
    core::SliceEase e2;
    e2.request(5.0f * kNotch);
    const float first = e2.step(kDt);
    expect(std::fabs(first + turned) < 1e-6f,
           "a cancelled request unwinds exactly what it had already turned");
}

void test_round_trip_is_exact() {
    // +d then -d, each drained fully, must net to zero - the property
    // --verify-4d's byte-identical rotation round-trip rests on.
    core::SliceEase a; a.request(0.25f);
    const float out = drain(a).total;
    core::SliceEase b; b.request(-0.25f);
    const float back = drain(b).total;
    expect(std::fabs(out + back) == 0.0f, "a rotation and its reverse net to zero");
}

void test_frame_rate_independence() {
    // The same turn at 30 Hz and at 144 Hz must arrive in about the same
    // wall-clock time, or the control changes feel with the frame rate.
    core::SliceEase slow; slow.request(8.0f * kNotch);
    const Drain ds = drain(slow, 1.0f / 30.0f);
    core::SliceEase fast; fast.request(8.0f * kNotch);
    const Drain df = drain(fast, 1.0f / 144.0f);
    const float ts = static_cast<float>(ds.frames) / 30.0f;
    const float tf = static_cast<float>(df.frames) / 144.0f;
    expect(std::fabs(ts - tf) < 0.15f,
           "settling time is set by the clock, not by the frame rate");
}

void test_zero_and_idle() {
    core::SliceEase e;
    expect(e.settled(), "starts settled");
    expect(e.step(kDt) == 0.0f, "an idle ease turns nothing");
    e.request(kNotch);
    expect(e.step(0.0f) == 0.0f, "a zero-length frame turns nothing");
    expect(!e.settled(), "and does not consume the debt");
}

}  // namespace

int main() {
    std::printf("slice_ease_tests: running...\n");
    test_pays_exactly_what_was_asked();
    test_no_frame_jumps();
    test_starts_immediately();
    test_opposite_requests_cancel();
    test_round_trip_is_exact();
    test_frame_rate_independence();
    test_zero_and_idle();
    std::printf("slice_ease_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
