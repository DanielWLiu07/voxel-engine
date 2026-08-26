#include "render/shader_set.h"

#include <cstdio>

namespace render {
namespace {

bool compile(gfx::Shader& s, const std::filesystem::path& root,
             const char* vert, const char* frag, const char* tag) {
    if (s.load((root / "shaders" / vert).string(),
               (root / "shaders" / frag).string())) return true;
    std::fprintf(stderr, "[shader] %s load failed\n", tag);
    return false;
}

}  // namespace

bool ShaderSet::load(const std::filesystem::path& root) {
    // Order is boot order, not draw order: nothing depends on it, and
    // reading it top to bottom should say what the engine can draw.
    return compile(terrain,    root, "basic.vert",        "basic.frag",          "terrain")
        && compile(sky,        root, "sky.vert",          "sky.frag",            "sky")
        && compile(shadow,     root, "shadow_depth.vert", "shadow_depth.frag",   "shadow")
        && compile(water,      root, "water.vert",        "water.frag",          "water")
        && compile(bright,     root, "fullscreen.vert",   "bright_extract.frag", "bright")
        && compile(bloom_down, root, "fullscreen.vert",   "bloom_down.frag",     "bloom_down")
        && compile(bloom_up,   root, "fullscreen.vert",   "bloom_up.frag",       "bloom_up")
        && compile(tonemap,    root, "fullscreen.vert",   "tonemap.frag",        "tonemap")
        && compile(wireframe,  root, "wireframe.vert",    "wireframe.frag",      "wireframe")
        && compile(crosshair,  root, "crosshair.vert",    "crosshair.frag",      "crosshair");
}

}  // namespace render
