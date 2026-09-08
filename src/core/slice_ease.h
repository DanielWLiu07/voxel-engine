#pragma once

#include <cmath>

namespace core {

// An angle the player has asked for, paid out over several frames.
//
// The wheel is a discrete control driving a continuous one. A notch used
// to be applied whole on the frame it arrived, and turning the 4D cut is
// not like turning a camera: --bench-4d measures one notch (0.003 rad)
// putting 19% of the visible columns somewhere else, and a trackpad flick
// arriving as eight notches in a single frame moving about half the
// window between two consecutive images. The result read as the world
// being swapped rather than turned.
//
// So the input writes an angle down here and the cut collects it over
// several frames. Two shapes at once:
//
//   - an exponential ease, so it starts immediately (about 28% of the
//     debt on the first 60 Hz frame - there is no input lag) and
//     decelerates into place rather than stopping dead
//   - a ceiling on angular velocity, which is what actually bounds the
//     visible jump: at 0.06 rad/s no frame moves more than a third of a
//     notch, about 6% of the columns against a whole notch's 19%
//
// Deliberately NOT capped at the rate the chunk stream can follow. That
// is near one notch a second, and a control capped there would need
// twelve seconds to turn the cut fourteen degrees. Geometry still lags a
// fast turn; what this fixes is the angle moving in visible steps.
//
// Exactness is a property this has to keep, not a nicety. The angle the
// player asked for must be the angle they get - every notch, summed,
// with nothing lost in the tail - because World::rotate_slice is exactly
// reversible and --verify-4d gates a byte-identical rotation round-trip
// on it. So the tail is paid off in one step once it falls below kSnap
// rather than being approached forever, and `owed` reaches exactly zero.
class SliceEase {
public:
    // Ask for another `radians` of turn. Signed; opposite requests cancel.
    void request(float radians) { owed_ += radians; }

    // Radians to turn this frame. Zero when nothing is outstanding.
    float step(float dt) {
        if (owed_ == 0.0f || dt <= 0.0f) return 0.0f;
        float s = owed_ * (1.0f - std::exp(-kRate * dt));
        const float cap = kMaxRadPerSec * dt;
        if (s >  cap) s =  cap;
        if (s < -cap) s = -cap;
        // The tail, and the reason `owed` lands on zero rather than
        // approaching it: below kSnap, or whenever the eased step would
        // overshoot, pay the remainder outright.
        if (std::fabs(owed_) <= kSnap || std::fabs(s) >= std::fabs(owed_)) {
            s = owed_;
        }
        owed_ -= s;
        // Pay the tail out, do not just forget it. Zeroing `owed_` here
        // without adding it to the step is what the first version did,
        // and it leaked a constant 7.4e-6 rad per request - a quarter of
        // a percent of one notch, invisible once and about four degrees
        // of drift over ten thousand of them. The angle asked for has to
        // be the angle turned.
        if (std::fabs(owed_) <= kSnap) { s += owed_; owed_ = 0.0f; }
        return s;
    }

    bool settled() const { return owed_ == 0.0f; }
    float outstanding() const { return owed_; }

    static constexpr float kRate         = 20.0f;   // 1/s, ease time constant
    static constexpr float kMaxRadPerSec = 0.06f;   // a third of a notch a frame
    static constexpr float kSnap         = 1e-5f;   // ~1/300 of a notch

private:
    float owed_ = 0.0f;
};

}  // namespace core
