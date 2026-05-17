#pragma once

#include "gfx/mesh.h"

namespace gfx {

// Build a unit cube centered at origin, 24 vertices (4 per face for correct
// per-face normals + UVs), 36 indices.
void build_unit_cube(Mesh& out);

}  // namespace gfx
