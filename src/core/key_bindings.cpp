#include "core/key_bindings.h"

#include <cstdio>

namespace core {

void print_bindings() {
    // Movement and mouse: continuous rather than a toggle, so the loop
    // reads them positionally and they are not in the table. This is the
    // one duplication here, and it is deliberate and confined to these
    // three lines.
    std::printf("[input] WASD = move, Space = jump (walk) / up (fly), "
                "LCtrl = down (fly), Shift = sprint\n");
    std::printf("[input] LClick = break, RClick = place\n");
    std::printf("[input] 1-8 = pick block (8 = Glow, a light source)\n");

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
