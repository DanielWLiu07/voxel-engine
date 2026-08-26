#pragma once

#include "gfx/shader.h"

#include <filesystem>

namespace render {

// Every shader program the engine compiles, named by what it draws rather
// than by the files it came from.
//
// This was ten locals and a ten-clause `if` in main, which made the
// inventory readable only as a wall of aligned strings and meant adding a
// pass meant editing a boolean chain. The pairing of a program with its
// two source files is the fact worth keeping in one place: it is the only
// thing that says which .vert goes with which .frag, and four of the ten
// share fullscreen.vert.
struct ShaderSet {
    gfx::Shader terrain;
    gfx::Shader sky;
    gfx::Shader shadow;
    gfx::Shader water;
    gfx::Shader bright;
    gfx::Shader bloom_down;
    gfx::Shader bloom_up;
    gfx::Shader tonemap;
    gfx::Shader wireframe;
    gfx::Shader crosshair;

    // Compiles all ten out of `root`/shaders. Returns false at the first
    // failure, having already named the program and let gfx::Shader report
    // the compiler log. Stops rather than continuing so the error a reader
    // sees first is the one that actually broke the boot.
    bool load(const std::filesystem::path& root);
};

}  // namespace render
