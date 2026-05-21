# voxel-engine

A desktop voxel engine in C++20 and OpenGL 4.1 Core. Greedy meshing, multithreaded chunk streaming, real-time shadow mapping, vertex AO, day/night, and a water plane with Fresnel. Measured on an Apple M4: 27.7x greedy triangle reduction on Perlin terrain, ~940 chunks/sec end-to-end pipeline.

## Screenshots

<!-- screenshot 1: terrain at noon, shadows on the ground, water visible at the horizon. drop file at docs/screenshots/noon.png -->
<!-- screenshot 2: sunset shot showing sky gradient + warm sun color hitting one face of the terrain. docs/screenshots/sunset.png -->
<!-- screenshot 3: ImGui debug HUD overlay with the perf panel visible. docs/screenshots/hud.png -->

## Highlights

- Greedy mesher producing 27.7x fewer triangles than naive face-culling on Perlin terrain. Area-correct against the naive output.
- Multithreaded chunk pipeline (terrain gen + greedy meshing) on a worker pool, ~940 chunks/sec end-to-end at radius 12 (625 chunks) on Apple M4.
- Infinite chunk streaming around the player. Chunks load and evict as the camera crosses chunk boundaries.
- Real-time directional shadow mapping with PCF filtering, dynamic light-space frustum fit, peter-panning bias.
- Per-vertex ambient occlusion baked into the greedy mesh.
- Day/night cycle with sun arc, sky gradient, and color ramps for sun, ambient, and fog.
- Distance fog blended against the sky for clean chunk-radius horizon.
- Animated water plane with Fresnel mixing and sine-wave displacement at sea level.
- AABB collision against the voxel grid in walk mode. DDA voxel raycast for break/place at 8-block reach.
- Dear ImGui debug HUD: live frame time, FPS, drawn chunks, triangles, pending async chunks, streaming counters, copy-to-clipboard snapshot.
- View-frustum culling against chunk AABBs.

## Measured performance

Apple M4 (10-core CPU), macOS 26.2 arm64, OpenGL 4.1 Apple renderer. Measured 2026-05-18.

| Metric | Value |
| --- | --- |
| Greedy meshing, Perlin terrain chunk | 3072 -> 111 quads (27.7x fewer tris, build 1.29 ms) |
| Greedy meshing, synthetic sine-bumped chunk | 1448 -> 496 quads (2.9x fewer tris, worst case) |
| Async chunk pipeline, radius 12 (625 chunks) | 663 ms wall, ~940 chunks/sec, 9 workers |
| Frustum cull ratio, gameplay viewpoint | 304 / 625 chunks drawn (~2.1x) |
| Frame time, radius 12, ~61k tris drawn | 8.5 ms (150 fps) |

Reproduce the mesher benchmark:

```
./build/voxel_engine --bench
```

## Build

Prerequisites: CMake 3.20+, Ninja, a C++20 compiler (Clang 15+ / GCC 12+ / MSVC 19.3+). First configure takes about two minutes while CMake FetchContent clones GLFW, GLM, and Dear ImGui. macOS, Linux, and Windows are supported. macOS is the primary dev target.

```
cmake -B build -G Ninja
cmake --build build -j
./build/voxel_engine
```

Build type defaults to Release. Pass `--bench` to run the mesher benchmark instead of opening a window.

## Controls

### Movement

| Key | Action |
| --- | --- |
| W A S D | Move |
| Space | Jump (walk mode) / up (fly mode) |
| Left Ctrl | Down (fly mode) |
| Left Shift | Sprint |
| F | Toggle walk / fly |

### Interaction

| Key | Action |
| --- | --- |
| Left click | Break block |
| Right click | Place block |

### View and debug

| Key | Action |
| --- | --- |
| Tab | Toggle mouse capture |
| F2 | Toggle ImGui HUD |
| F5 | Save world to `./saves/world1/` |
| F6 | Reload world from `./saves/world1/` |
| F12 | Screenshot to `./screenshots/` |
| C | Copy perf snapshot to clipboard |
| T | Pause / resume time of day |
| `[` / `]` | Step time of day backward / forward |
| V | Toggle vsync |
| Esc | Quit |

## Architecture

Layered, no globals. `gfx/` is a generic OpenGL wrapper that knows nothing about voxels. `world/` owns voxel data and meshing. `render/` composes draw passes from `gfx/` and `world/`. `game/` is the only layer that coordinates player input with world state. `ui/` is the debug HUD. Chunk generation and meshing run on a worker pool; all OpenGL calls stay on the main thread.

```
src/
  core/    window, input, thread pool
  gfx/     shader, mesh, camera, frustum, shadow map, water
  world/   chunk, terrain gen, greedy mesher, world container, streaming
  render/  lighting, draw passes (shadow, sky, terrain, water)
  game/    player, AABB physics, block interaction
  ui/      debug HUD
  main.cpp
shaders/   GLSL 4.10 core
third_party/  glad, stb, FastNoiseLite (vendored)
```

Dependencies via CMake FetchContent: GLFW, GLM, Dear ImGui. Vendored: GLAD (GL 4.1 core loader), stb_image, stb_image_write, FastNoiseLite.
