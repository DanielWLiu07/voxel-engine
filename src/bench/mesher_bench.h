#pragma once

namespace bench {

// The headless benchmark behind `voxel_engine --bench`: greedy vs naive
// meshing, frustum and section culling, section-graph occlusion, and the
// whole-world GPU mesh footprint.
//
// It lives outside main.cpp because CI depends on it. The gate in
// .github/workflows/ci.yml parses this function's stdout, so its output is
// a contract with something outside the program, and a contract is easier
// to keep when it is not buried three hundred lines into an entry point
// that also opens a window.
//
// Nothing here touches GL. Every number is CPU-side arithmetic over
// generated terrain, deterministic for a given seed, which is what lets
// the same figures reproduce on a CI runner with no GPU at all.
//
// `stream_radius` is passed in rather than baked in because the engine's
// default streaming radius belongs to the engine, not to its benchmark.
// Returns EXIT_SUCCESS.
int run_mesher_bench(int stream_radius);

}  // namespace bench
