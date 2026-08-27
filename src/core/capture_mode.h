#pragma once

namespace core {

// Which scripted run, if any, is driving this frame.
//
// The engine has four capture-ish modes and two distinct questions get
// asked about them, which used to be spelled out inline at four sites in
// the render loop with the terms in a different order each time and the
// membership quietly different:
//
//   shot_after > 0 || orbit_frames > 0 || cycle_frames > 0
//   shot_after > 0 || orbit_frames > 0 || cycle_frames > 0 || bench_frames > 0
//
// The difference is whether --bench-frame counts, and it does for one
// question and not the other. Nothing said so, and a fifth mode would have
// had to be added to four expressions correctly to avoid a subtle wrong
// frame in one of them.
//
// Naming the two questions is the whole point of this type.
struct CaptureMode {
    int shot_after   = 0;  // frames to settle before a single PNG
    int orbit_frames = 0;  // frames of a camera orbit, one PNG each
    int cycle_frames = 0;  // frames of a day/night cycle, one PNG each
    int bench_frames = 0;  // frames to time; writes no image

    // The camera is driven by a script rather than by the player, so live
    // mouse and keyboard input must not steer it - a capture that responds
    // to a stray mouse move is not reproducible. Also what pins the water
    // phase, so a shot stays diffable and an orbit's last frame meets its
    // first.
    //
    // --bench-frame is NOT here: the plain frame bench uses the player's
    // pose and the orbit bench drives the camera through its own path.
    bool scripted_camera() const {
        return shot_after > 0 || orbit_frames > 0 || cycle_frames > 0;
    }

    // The run exists to produce an image or a measurement, so interface
    // elements - the crosshair, the block selection outline - are
    // suppressed. They are interface, not scene, and they were quietly
    // appearing in the middle of every documentation image.
    //
    // --bench-frame IS here: the raycast the selection outline needs is
    // work, and timing it would measure something the capture does not do.
    bool suppresses_interface() const {
        return scripted_camera() || bench_frames > 0;
    }

    // Time of day is held still. A transition mid-run would fire the
    // shadow-resync refresh (a timing spike in a bench) or change the
    // lighting between two frames meant to be compared.
    bool pins_time_of_day() const { return suppresses_interface(); }

    // Frames of a multi-frame PNG capture, or 0 when this is not one.
    // Orbit wins if both are somehow set, which matches the order the
    // camera path is chosen in.
    int image_sequence_frames() const {
        return orbit_frames > 0 ? orbit_frames : cycle_frames;
    }
};

}  // namespace core
