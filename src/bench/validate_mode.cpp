#include "bench/validate_mode.h"

#include <cstdio>

namespace bench {

ModeResult run_validate(world::World& wrld, const world::TerrainGen& terrain,
                        core::ThreadPool& pool) {
        // Measured BEFORE the switch test below, which re-meshes the
        // window twice and leaves more chunks with neighbours to cull
        // against than a first load has. The reported figure has to
        // describe the world the flags asked for, not the world the
        // validator left behind.
        const int    bad    = wrld.debug_validate_gpu_meshes();
        const double gpu_mb = static_cast<double>(wrld.resident_gpu_bytes())
                              / (1024.0 * 1024.0);

        // In prism mode, validate the SWITCH as well as the state.
        //
        // The mesher can be toggled at runtime (P), and the position
        // encoding is per chunk precisely so a half-switched window
        // draws correctly. That is a claim about a transient, and a
        // transient is exactly what no screenshot catches: flip the
        // mesher, let the window refill part of the way, and validate
        // the world that exists while both kinds are resident. A chunk
        // built the wrong way round is crushed into a sixteenth of its
        // footprint, which the range check sees.
        if (wrld.prism_meshing()) {
            wrld.set_prism_meshing(false);
            wrld.resample_slice(terrain, pool);
            // Drain part of the way, not all of it. A fully drained
            // window is not mixed, and validating one would prove
            // nothing - the counts below are what says this ran
            // against a window that really did hold both encodings.
            //
            // Drained to a CONDITION rather than for a fixed number of
            // rounds. A fixed count makes the mix a function of how
            // fast the workers happen to be, which is a gate that
            // passes on this machine and fails on a slower one for no
            // reason anybody could act on.
            int cube = 0, prism = 0;
            for (int i = 0; i < 20000 && cube < 8; ++i) {
                wrld.drain_finished(8);
                wrld.mesh_encoding_mix(&cube, &prism);
                if (cube == 0) std::this_thread::yield();
            }
            const int mixed_bad = wrld.debug_validate_gpu_meshes();
            std::printf("[validate] mid-switch: %d cube + %d prism "
                        "meshes resident, %d flagged\n",
                        cube, prism, mixed_bad);
            wrld.set_prism_meshing(true);
            wrld.resample_slice(terrain, pool);
            for (int i = 0; i < 64; ++i) wrld.drain_finished(256);
            if (mixed_bad > 0 || cube == 0 || prism == 0) {
                std::printf("\nVALIDATE prism switch FAILED (%s)\n",
                            mixed_bad > 0
                                ? "flagged triangles"
                                : "the window never held both encodings, "
                                  "so nothing was checked");
                return ModeResult::Failed;
            }
        }
        // The engine's own resident mesh footprint, printed here
        // because this is the only headless mode that builds a real
        // world on a real GPU. --bench computes the same figure from
        // the mesher alone; the two agreeing is what says the
        // streaming path is uploading what the mesher produces.
        // 11,528,256 bytes on the default seed and radius, and it
        // reproduces exactly - better than twenty consecutive runs,
        // including immediately after a full audit.
        //
        // It printed 11.00 twice during one session and never
        // reproduced in isolation. Both times another process was
        // building into the same build/ directory, so the likeliest
        // explanation is that those runs used a binary that was being
        // relinked underneath them rather than that the figure moves.
        // Recorded rather than asserted, because the difference
        // between "deterministic" and "deterministic except twice" is
        // exactly the kind of thing this repo does not round off.
        //
        // The figure the CI gate actually bounds is world_mesh_mb from
        // --bench, which check_invariance proves byte-identical across
        // runs; this one corroborates it from the running engine.
        std::printf("\nVALIDATE chunks=%zu bad_triangles=%d "
                    "gpu_mesh_mb=%.2f %s\n",
                    wrld.chunk_count(), bad, gpu_mb,
                    bad == 0 ? "ok" : "FAILED");
        if (bad > 0) return ModeResult::Failed;
        return ModeResult::Finished;
}

}  // namespace bench
