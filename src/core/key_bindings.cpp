#include "core/key_bindings.h"

#include <cstdio>

namespace core {

void print_bindings() {
    // Movement and mouse: continuous rather than a toggle, so the loop
    // reads them positionally and they are not in the table. This is the
    // one duplication here, and it is deliberate and confined to these
    // three lines.
    // Stated first because it is the one thing that makes every other
    // binding work, and its absence is invisible: movement is gated on the
    // cursor being captured, so without it WASD silently does nothing.
    std::printf("[input] mouse captured - Tab releases it "
                "(movement needs it captured)\n");
    std::printf("[input] WASD = move, Space = jump (walk) / up (fly), "
                "LCtrl = down (fly), Shift = sprint\n");
    std::printf("[input] LClick = break, RClick = place\n");
    std::printf("[input] 1-8 = pick block (8 = Glow, a light source)\n");
    // Held like the movement keys, not tapped like the table's toggles -
    // w is an axis you travel along, so it belongs with WASD in spirit
    // even though the table is where its bindings live.
    std::printf("[input] E / Q = travel along w, the fourth axis "
                "(hold; Shift sprints)\n");
    // The two rotation planes, spelled out because the second one is
    // invisible otherwise: a player who only ever scrolls will never
    // discover that the world can also turn sideways.
    std::printf("[input] SCROLL WHEEL = turn your 3D slice through 4D, "
                "ZW plane (this is the one that reshapes the world)\n");
    std::printf("[input] HOLD M or MIDDLE MOUSE = the mouse turns the cut "
                "instead of the camera:\n");
    std::printf("[input]     up/down = ZW (same as the wheel), "
                "left/right = XW (the plane the wheel cannot reach)\n");

    // Four to a line, so the list stays scannable as bindings are added.
    // The separator is written before each entry rather than after, which
    // is what keeps a dangling "|" off the end of a partial last line.
    constexpr int kPerLine = 4;
    int on_line = 0;
    for (const auto& b : kBindings) {
        if (on_line == 0) std::printf("[input] ");
        else              std::printf(" | ");
        std::printf("%s = %s", b.shown, b.action);
        if (++on_line == kPerLine) { std::printf("\n"); on_line = 0; }
    }
    if (on_line != 0) std::printf("\n");
}

}  // namespace core
