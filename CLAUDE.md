# CLAUDE.md

Context for Claude Code sessions on this project. Read first before making changes.

## What this is

A desktop voxel game engine in C++/OpenGL 4.5, built solo as a resume project for generalist SWE roles (FAANG / infra / systems — not graphics specialist). Started 2026-05-17 with a 3-week intense timeline.

The headline value is **measurable perf wins**, not visual polish. Every architectural choice is judged by whether it produces a number a non-graphics interviewer can grade.

## Locked scope

**In:** greedy meshing, 16×256×16 chunks, frustum culling, multithreaded chunk gen w/ Perlin noise, basic Phong lighting, block place/break, ImGui debug HUD with perf metrics.

**Stretch (only after MVP solid):** cascaded shadow maps, vertex AO, day/night cycle.

**Out (do not propose; user already considered and rejected):**
- MediaPipe / pose tracking / boxing pivot — different project, no shared story with a voxel engine
- Browser / WebGL port — different runtime, different tooling, mostly a rewrite
- Networking / multiplayer — different project
- Scripting languages, databases, Docker, "AI" — irrelevant to a voxel engine

If the user proposes any of the above mid-project, see `~/.claude/projects/<this-project>/memory/feedback_scope_creep.md` and surface the trade-offs before agreeing.

## Tech stack

- C++20, CMake (3.20+), Ninja, deps via CMake FetchContent (no vcpkg)
- GLFW (window/input), GLAD (GL loader), GLM (math), stb_image (textures)
- Dear ImGui (debug HUD, tweakables)
- spdlog (logging), FastNoise2 (terrain)
- **OpenGL 4.1 Core Profile** — macOS caps GL at 4.1 (Apple deprecated GL in 2018 and froze it). No DSA, no compute shaders, no `glMultiDrawIndirect`. Use bind-to-modify style throughout.
- macOS is the primary dev target (user's machine); code stays portable to Linux/Windows but we don't use features above 4.1.

## Architecture (clean layering — do not violate)

```
src/
  core/    Window, Input, Time, Logger          (depends on: nothing project-specific)
  gfx/     Shader, Texture, Mesh, Camera        (depends on: core)
  world/   Block, Chunk, ChunkMesher, World     (depends on: gfx, core)
  game/    Player, Physics, BlockInteraction    (depends on: world, gfx, core)
  ui/      DebugHud                              (depends on: all, used at top level)
  main.cpp
shaders/   *.vert *.frag
assets/    textures/, configs/
```

Rules:
- `gfx/` knows nothing about voxels — it's a generic OpenGL wrapper layer
- `world/` owns voxel data and meshing; it uses `gfx/` to upload meshes but never reaches into gameplay
- `game/` is the only layer allowed to coordinate world + player + input
- No globals. Singletons only for: Logger.

## Conventions

- C++20 features welcome (concepts, ranges where they help readability, `std::span`, designated initializers)
- RAII for all GPU resources — destructors call `glDelete*`
- `snake_case` for files and functions, `PascalCase` for types, `kCamelCase` for constants
- No exceptions in hot paths; use `std::expected` or sentinel returns
- Asserts liberally in debug, compiled out in release
- Pass `const T&` for non-trivial types, value for primitives, `T&&` only when you mean it
- Shader files are plain GLSL with `#version 410 core`; no preprocessor hacks unless we need #include

## Perf rules (this is what the project sells on)

- Always measure before claiming a win. `Tracy` is on the Tier 1 add list — once wired, every perf number on the resume should be reproducible from a Tracy capture.
- Don't pre-optimize. Build the naive version, measure, then optimize the hot path.
- The greedy-mesher win is the headline. Benchmark naive vs. greedy and log the ratio — that's the bullet.
- Chunk gen runs on a worker pool. GL calls run on the main thread only. Crossing this boundary is the most likely source of bugs.

## Resume bullets (the artifact we're optimizing for)

Reconciled to measured, reproducible numbers (Apple M4, radius 12). Every
figure maps to a command in the README / `docs/bench/`; none are aspirational.

1. Built a C++20 / OpenGL 4.1 voxel engine that streams a **40M-voxel world**
   (625 chunks resident) at **175 FPS** (5.7 ms/frame, 29M triangles/sec,
   253 MB RSS); a greedy meshing pass merges coplanar faces for **18× fewer
   triangles** than naive culling, guarded by a CI regression gate.
2. Engineered off-thread chunk streaming — a worker pool generates terrain +
   meshes while the main thread owns all GPU uploads — sustaining **2,200
   chunks/sec at 8.4× parallel efficiency** on 9 workers; hierarchical frustum
   + occlusion culling cuts drawn sections up to **70× underground**.
3. (Systems-role optional) Implemented a **ThreadSanitizer-clean lock-free
   MPMC queue** (2–5× faster than a mutex queue under contention) and kept the
   simpler mutex pool after measuring the queue is never the bottleneck at the
   engine's ~1 ms job granularity — concurrency validated under TSan + ASan/
   UBSan in CI, the decision Tracy-profiled and benchmark-backed.

Why the old draft changed: it claimed "lock-free streaming" (the live path is a
mutex pool — see bullet 3 for the honest version), "30×" greedy (CI-gated number
is 18.1×; 27.7× was a cherry-picked single-biome case), "~50 chunks/sec"
(measured 2,226), "144 FPS" (measured 175), and "8× cull" (matches no measured
ratio; real figures are ~3× frustum / 12× section / up to 70× occlusion). An
interviewer who probes a number that collapses is the failure mode we optimize
against — every bullet above survives "show me."

Every PR should ask: does this make these bullets more defensible, or is it scope creep?

## Build (will be filled in by task #3)

```
# vcpkg bootstrap + cmake configure/build commands go here once task #3 lands
```

## Useful pointers for future Claude sessions

- Memory at `~/.claude/projects/-Users-danielwliu-Dev-projects-2026-opengl-game-test/memory/` has user background, locked goal, and scope-creep feedback. Read it.
- The user is solid in C++ but new to graphics — explain GL concepts from first principles, don't assume VBO/VAO/uniform familiarity.
- Keep responses tight. User wants to ship, not read essays.
