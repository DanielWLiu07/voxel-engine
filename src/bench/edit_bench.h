#pragma once

#include "bench/validate_mode.h"   // bench::ModeResult
#include "world/world.h"

namespace bench {

// --bench-edit N: time N block breaks end to end.
//
// Each sample covers everything a break costs on the main thread - light
// re-propagation, the greedy re-mesh, the section re-bucket, the GL
// re-upload and the visibility recompute - which is why it is timed here
// rather than around World::set_block alone.
//
// Positions walk a deterministic lattice at underground depths that are
// solid on any seed, so a break never no-ops and the sample count is the
// count asked for.
//
// `stream_radius` is reported rather than used: an edit's cost depends on
// how much world is resident around it, so the figure is meaningless
// without it.
ModeResult run_edit_bench(world::World& wrld, int edits, int stream_radius);

}  // namespace bench
