#pragma once

#include "core/thread_pool.h"
#include "world/terrain_gen.h"
#include "world/world.h"

namespace bench {

// What a one-shot mode wants the engine to do next.
//
// These modes are not the render loop. They run once, print a line a
// script greps for, and end the program - and they had grown to 1,628
// lines inside main's loop, which is more than half of it. Returning an
// intent rather than calling glfwSetWindowShouldClose or returning
// EXIT_FAILURE from inside is what lets them live outside main without
// also owning the window or the process.
enum class ModeResult {
    Finished,   // did its job; the program should end successfully
    Failed,     // the check it performs did not hold
};

// --validate: read every mesh back off the GPU and check it against the
// voxel data that produced it, then do the same across a live switch of
// the mesher. Prints the VALIDATE line the audit and CI grep for.
//
// Takes what it touches and nothing else - a world to check, and the
// terrain and pool it needs to re-mesh during the switch test. It does
// not know there is a window.
ModeResult run_validate(world::World& wrld, const world::TerrainGen& terrain,
                        core::ThreadPool& pool);

}  // namespace bench
