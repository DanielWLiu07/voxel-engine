#pragma once

#include <string>

namespace gfx {

// Reads the current default framebuffer (rect 0,0,w,h) and writes a PNG.
// Returns the path written on success, empty string on failure. filename
// empty = timestamped name (F12 path); non-empty = exact name, so scripted
// captures (--screenshot-after) are diffable run to run.
std::string save_screenshot(int w, int h, const std::string& dir = "./screenshots",
                            const std::string& filename = "");

}  // namespace gfx
