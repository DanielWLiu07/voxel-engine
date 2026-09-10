# How the engine works, from scratch

Written to be read start to finish by someone who knows C++ but has
never written graphics code. Every term is defined the first time it
appears. The last two sections cover the numbers on the résumé: what
each one means, how it was measured, and which have moved since.

---

## 0. The one-paragraph version

A voxel world is a 3D array of block IDs. Drawing it naively means
drawing millions of cubes, which no GPU can do at 60 frames per second.
So the engine never draws cubes. It converts the array into a *surface* -
only the faces a camera could possibly see - merges neighbouring faces
into the largest rectangles it can, packs each corner of those rectangles
into 12 bytes, and hands the result to the GPU once. Generating that
surface is slow, so it happens on nine background threads while the main
thread draws. Everything else in the engine is a variation on one of
those four ideas.

---

## Part 1: What the world actually is

A **voxel** is a volume pixel: one cube of the world, one block. It is
not an object with a position. It is a cell in a grid, and its position
is implied by *where it sits in the array*.

```cpp
// A chunk is a fixed-size box of the world.
// 16 wide, 256 tall, 16 deep = 65,536 voxels.
std::array<std::uint8_t, 16 * 256 * 16> blocks;
```

One byte per voxel. The byte is a block ID: 0 = air, 1 = stone, 2 = dirt,
and so on. That is the whole data structure. A 65,536-voxel chunk costs
64 KB of RAM.

**Why not store a position per block?** A position would be 12 bytes
(three floats) against the 1 byte the ID costs, and it would be
redundant: index `i` in the array *is* the position, recoverable with
three divisions. In a structure with millions of entries, that ratio is
the difference between a world that fits in RAM and one that does not.

**Why 16 × 256 × 16?** The 256 is the world height - the whole vertical
column lives in one chunk, so nothing has to be stitched vertically. The
16s are a compromise. Smaller chunks mean more of them, and each one
costs a separate instruction to the GPU. Larger chunks mean rebuilding
more work when a single block changes. 16 is the value Minecraft settled
on and the reasoning is the same.

The world is a hash map of chunks keyed by 2D coordinate:

```cpp
std::unordered_map<ChunkCoord, std::unique_ptr<Slot>> chunks_;
```

"Render distance 12" means loading every chunk within 12 of the player on
each axis: a 25 × 25 square, **625 chunks**, and

```
625 × 65,536 = 40,960,000 voxels
```

That is the 40-million-voxel world on the résumé. It is not an estimate;
it is 625 × 16 × 256 × 16.

---

## Part 2: How a GPU draws anything

Skip this if you know it. Everything after depends on it.

### Triangles are the only primitive

A GPU does not know what a cube is. It knows how to fill in **triangles**.
Every shape you have ever seen rendered in 3D was triangles. A square
face is two triangles sharing an edge:

```
  0───────3        triangle A: 0, 1, 2
  │ ╲   A │        triangle B: 0, 2, 3
  │   ╲   │
  │ B   ╲ │        (0,1,2,3 are the four corners)
  1───────2
```

A cube has 6 faces × 2 triangles = **12 triangles**, and each triangle
has 3 corners, so 36 corner-slots - though only 8 distinct corners exist.

### A vertex is a corner with attributes

A **vertex** is one corner of a triangle plus everything the GPU needs to
know about it. Position, certainly. But also:

- a **normal** - the direction the surface faces, used for lighting. A
  face pointing at the sun is bright; one pointing away is dark.
- **texture coordinates** (`u`, `v`) - where in the texture image this
  corner samples from
- whatever else you want: a block ID, a light level, an ambient occlusion
  value

### The three buffers

You cannot hand the GPU a `std::vector` on the CPU. GPU memory is
separate. You copy data into GPU-side buffers and then refer to them.

- **VBO** (Vertex Buffer Object) - a flat array of vertex bytes in GPU
  memory. This is where the geometry lives.
- **EBO** (Element Buffer Object, also called an index buffer) - a list of
  *indices into the VBO*. This is what lets a cube store 8 vertices
  instead of 36: the index buffer says "triangle 1 uses vertices 0, 1, 2;
  triangle 2 uses 0, 2, 3", reusing shared corners.
- **VAO** (Vertex Array Object) - not data. It is a saved *description*:
  "the VBO's bytes are laid out with position at offset 0 as two unsigned
  bytes, normal at offset 2, ..." Binding a VAO restores that whole
  description in one call instead of a dozen.

### Shaders

Two small programs run on the GPU for every draw.

The **vertex shader** runs once per vertex. Its job is to turn a position
in world space into a position on screen, by multiplying by matrices that
encode where the camera is and how it is angled.

The **fragment shader** runs once per *pixel covered by a triangle* -
often millions of times a frame. It computes the final colour: sample the
texture, apply lighting, apply fog.

A **uniform** is a value constant across a draw call that both shaders can
read - the camera matrix, the sun direction, the time of day.

### The draw call

```cpp
glBindVertexArray(vao);
glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, nullptr);
```

That is one **draw call**: "draw this many triangles from the currently
bound buffers." Draw calls have fixed overhead - the driver validates
state, the command goes to the GPU - so **the number of draw calls per
frame matters as much as the number of triangles**. Ten thousand tiny
draw calls will be slower than one large one even if the triangle count
is identical. This is why chunks exist as a unit: one chunk, one mesh,
one draw call.

---

## Part 3: Why the naive approach cannot work

Take the 40-million-voxel world and draw each block as a cube.

```
40,960,000 voxels × 12 triangles = 491,520,000 triangles
```

Half a billion triangles per frame. At 60 fps that is 29 billion
triangles per second. A modern GPU does single-digit billions. You are
**three to four orders of magnitude** over budget.

And the memory:

```
491,520,000 triangles × 3 vertices × 40 bytes/vertex ≈ 59 GB
```

The world does not fit in GPU memory, never mind draw. So the naive
version is not slow. It is impossible. Every technique below is about
closing that gap.

---

## Part 4: The first cut - only draw the surface

Here is the observation that does most of the work: **you cannot see
inside solid rock.**

If a block has a solid block on all six sides, it contributes nothing to
any image, from any camera position. It is interior. And in a voxel world
generated from terrain noise, the overwhelming majority of blocks are
interior - a mountain is solid all the way through.

So the mesher walks the chunk and, for each block, checks its six
neighbours:

```cpp
for each solid block at (x, y, z):
    for each of the 6 directions:
        if the neighbour in that direction is air:
            emit a face here      // visible surface
        else:
            emit nothing          // buried, can never be seen
```

This is **hidden face removal**, and it converts a volume problem into a
surface problem. A 16 × 256 × 16 chunk of solid stone has 65,536 blocks
and exactly 0 visible faces if surrounded by other stone chunks. Terrain
in practice keeps a thin skin of faces over a solid interior.

### The subtlety that cost real numbers

What about blocks at the *edge* of a chunk? Their neighbour lives in the
next chunk over, which may not be loaded yet.

The original engine took the easy answer: treat "outside this chunk" as
air, and emit the face. That is safe - you never get a hole - but it
means **every shared boundary face gets built twice**, once by each
chunk, and both are buried between two solid chunks where no camera can
reach them.

Fixing that ("cross-chunk face culling") meant passing each mesher job a
copy of the four boundary layers of its neighbours. It removed 1.4 million
quads of geometry nobody could see. It also **changed the greedy-meshing
ratio on the résumé**, and Part 13 covers why.

---

## Part 5: Greedy meshing - the headline optimization

After hidden-face removal you have a set of unit squares. Now merge them.

Consider a flat 4 × 4 patch of grass, seen from above. Hidden-face removal
gives 16 separate 1 × 1 quads:

```
┌─┬─┬─┬─┐
├─┼─┼─┼─┤     16 quads = 32 triangles = 64 vertices
├─┼─┼─┼─┤
├─┼─┼─┼─┤
└─┴─┴─┴─┘
```

But it is one flat square. It could be **one quad**:

```
┌───────┐
│       │     1 quad = 2 triangles = 4 vertices
│       │
│       │
└───────┘
```

16× less geometry, pixel-for-pixel identical on screen. That is greedy
meshing: merge coplanar, adjacent faces that share all their attributes
into the largest rectangles you can.

### The algorithm

Work one 2D slice at a time. For each slice, build a grid of "what face
is here, if any", then:

1. Scan for the first unvisited face.
2. Extend right as far as faces match (same block, same light, same AO,
   same facing).
3. Extend down as far as **entire rows** match.
4. Emit that rectangle as one quad; mark all its cells visited.
5. Repeat.

```
Step 2: extend right          Step 3: extend down
┌───────┬─┬─┐                 ┌───────┬─┬─┐
│▓▓▓▓▓▓▓│ │ │                 │▓▓▓▓▓▓▓│ │ │
├─┬─┬─┬─┼─┼─┤                 │▓▓▓▓▓▓▓│ │ │
│ │ │ │ │ │ │                 ├───────┼─┼─┤
└─┴─┴─┴─┴─┴─┘                 └───────┴─┴─┘
```

"Greedy" because it takes the largest rectangle available at each step
without backtracking to look for a globally optimal tiling. Optimal
tiling is far more expensive and buys very little.

### Why the attributes must match

Two faces can only merge if they are *indistinguishable* on screen. Same
block type (or the texture would change mid-quad), same light level (or
the shading would), same ambient occlusion, same facing direction. That
is why a quad stops at a shadow boundary - and why the ratio you achieve
depends on how much detail your lighting adds.

### What it is worth

**5.33× fewer triangles**, measured, gated in CI at ≥ 4.5×.

Terrain merges well because it is locally flat: large runs of grass, long
flat stone faces, big water surfaces.

### Proving it is correct

A mesher bug is nasty: it produces a *plausible* world with a hole in it
that only appears from one angle. So the engine has a **differential
fuzz test**. It meshes the same chunk two ways - the naive one-quad-per-
face mesher and the greedy one - decomposes every quad from both back
into unit 1 × 1 faces, and compares the two as *sets*.

They must be identical: same cells covered, same facing, same block ID,
no duplicates and no gaps. Run over 180 combinations of fill pattern ×
neighbour configuration × random seed. If greedy meshing ever changes
which surface exists rather than only how it is packed, this fails.

---

## Part 6: Vertex packing - 40 bytes to 12

The obvious vertex layout uses floats for everything:

```cpp
struct Vertex {          // the version this replaced
    glm::vec3 position;  // 12 bytes
    glm::vec3 normal;    // 12 bytes
    glm::vec2 uv;        //  8 bytes
    float ao;            //  4 bytes
    float block_id;      //  4 bytes
};                       // = 40 bytes
```

Every field is wasteful, and voxels are what make it obvious:

- **Position** is a float triple, but block corners land on integers
  inside a chunk. `x` and `z` need 0..16. That is one byte each. `y`
  needs 0..256, two bytes.
- **Normal** is a direction vector, but a cube face points along exactly
  one of six axes. Store an *index* 0-5 into a lookup table: one byte.
- **AO** is a float, but it has four levels. One byte.
- **Block ID** is a float, but there are fewer than 256 block types. One
  byte.

The result:

```cpp
struct VertexPacked {
    std::uint8_t  x, z;        // 0..16, chunk-local
    std::uint8_t  normal;      // index into a table of 6 directions
    std::uint8_t  ao;          // 0 occluded .. 3 unoccluded
    std::uint16_t y;           // 0..256
    std::uint16_t u, v;        // texture coordinates
    std::uint8_t  block_id;
    std::uint8_t  light;       // 0..15
};
static_assert(sizeof(VertexPacked) == 12);
```

**3.3× smaller.** The vertex shader unpacks it - integer attributes are
converted on the way in, and the normal index becomes a direction with a
constant array lookup.

Two details worth being able to explain:

- The `light` byte was **free**. The struct needed 12 bytes for alignment
  whether that field existed or not, so per-vertex block lighting cost
  zero bytes.
- Positions are **chunk-local**, which is why one byte suffices. The
  chunk's world position is passed once per draw call as a uniform and
  added in the vertex shader.

Smaller vertices are not only about capacity. GPUs are usually
**memory-bandwidth bound**, not arithmetic bound: the cost is moving
bytes, not doing maths on them. Cutting vertex size 3.3× cuts the bytes
the GPU must read per frame by 3.3×.

---

## Part 7: The shared index buffer

Every quad's index pattern is the same:

```
0, 1, 2,  0, 2, 3      // first quad
4, 5, 6,  4, 6, 7      // second quad
8, 9,10,  8,10,11      // third quad
```

Quad *q* always uses `4q, 4q+1, 4q+2, 4q, 4q+2, 4q+3`. It never depends
on the chunk's contents - only on how many quads there are.

So there is no reason for each chunk to own an index buffer. The engine
builds **one** buffer containing that pattern for the largest mesh it has
seen, and every chunk binds it and draws the first `6 × quad_count`
entries.

```cpp
for (std::size_t q = 0; q < capacity; ++q) {
    const auto b = static_cast<std::uint32_t>(4 * q);
    pattern.insert(pattern.end(), {b, b+1, b+2, b, b+2, b+3});
}
```

Per-chunk index memory goes to **zero**, and 625 chunks stop each holding
their own copy of an identical array. Worth **1.50×** on the total.

---

## Part 8: The three wins, multiplied

Measured at radius 12, 625 chunks, each row adding one optimization:

| | Quads | GPU mesh |
| :--- | ---: | ---: |
| naive faces, 40 B vertex, per-chunk indices | 722,030 | 126.7 MB |
| + greedy meshing | 237,688 | 41.7 MB |
| + 12-byte packed vertex | 237,688 | 16.3 MB |
| + one shared index buffer | 237,688 | **10.9 MB** |

Face merging 3.0×, vertex packing 2.56×, index sharing 1.50×. Together
**11.6×**, from 126.7 MB to 10.9 MB.

Note the quad column stops changing after the first row. Packing and
index sharing do not remove geometry; they make the same geometry
cheaper. Only merging removes it. Being able to say which optimization
did what is the point of measuring them separately.

---

## Part 9: Threading - the 9-worker pipeline

Generating a chunk is expensive. For each of 256 columns you evaluate
several octaves of Perlin noise for the height, more for caves, more for
biome and temperature, then fill 256 blocks of column, then mesh the
whole thing. Call it 5 ms per chunk.

Loading 625 chunks serially: **3+ seconds of frozen window**. Unacceptable.

But this work is *embarrassingly parallel*. Chunk A's terrain does not
depend on chunk B's. So the engine runs a thread pool:

```
workers = hardware_concurrency() - 1     // 9 on a 10-core M4
```

Minus one so the main thread keeps a core to itself.

### The rule that makes it safe

**OpenGL calls happen on the main thread only.** An OpenGL context is
bound to one thread; calling `glBufferData` from a worker is undefined
behaviour, and it will often appear to work before corrupting something
much later.

So the pipeline splits:

```
WORKER THREAD                      MAIN THREAD
─────────────────────              ──────────────────────
generate terrain (noise)
build the mesh (greedy)
  ↓
produce a plain std::vector
of VertexPacked
  ↓
push onto a finished queue  ────►  pop from the queue
                                   glBufferData(...)   ← the only GL
                                   draw
```

The worker's output is **pure data** - a vector of bytes with no GPU
handles in it. The main thread performs the upload. That boundary is the
single most likely source of bugs in an engine like this, which is why it
is stated as a rule rather than left to judgement.

### Measuring parallel efficiency

Sum the wall-clock time each worker spent, divide by the wall-clock time
the whole load took:

```
worker total 14,059 ms ÷ 1,584 ms elapsed = 8.9× across 9 workers
```

8.9 out of a theoretical 9. In practice it ranges roughly **6.3× to
8.5×** depending on what else the machine is doing - it is a timing, so
it moves. Quote it as a range or say what load it was taken under.

### The queue that was built and then not shipped

A lock-free multi-producer/multi-consumer queue exists in the repo,
cache-line padded to avoid false sharing, ThreadSanitizer-clean, and 2-5×
faster than a mutex-guarded `std::queue` under contention.

It is **not** what the engine uses. Profiling showed jobs take about a
millisecond, so the queue is touched roughly a thousand times a second -
a rate at which a mutex is not remotely a bottleneck. The simpler mutex
pool shipped.

That is a good thing to be able to say out loud: the measurement said the
clever thing was not needed, so the simple thing stayed.

---

## Part 10: Culling - not drawing what you cannot see

Meshing decides what geometry *exists*. Culling decides what to *draw*
this frame. Three layers, cheapest first.

### Frustum culling

The camera sees a truncated pyramid - the **view frustum**. Extract its
six planes from the camera matrix, and for each chunk test its bounding
box against them. Outside any plane means invisible, so skip the draw
call entirely.

Cheap (six dot products per chunk) and worth about **3×**, since roughly
two-thirds of loaded chunks are behind or beside you.

### Section culling

A chunk is 256 tall, but terrain occupies a band of that. Split each
chunk vertically into 8 **sections** of 32 blocks and track which contain
geometry. Empty sky and solid bedrock never get drawn. Worth about
**12×** on top of frustum culling.

### Occlusion culling

The interesting one. Standing in a cave, you should not draw the terrain
on the other side of the rock - it is in the frustum, and it is
non-empty, but solid stone is between you and it.

For each section, flood-fill its air at generation time and record which
of its six faces are mutually connected. That is 15 bits - one per
unordered pair of the 6 faces:

```cpp
// Bit set = a sightline can pass through this section's air
// between those two faces.
using SectionVisMask = std::uint16_t;   // 15 bits, C(6,2) = 15 pairs
```

A fully solid section is `0` (no sightlines). A fully empty one is
`0x7FFF` (all pairs connected). Then flood-fill *across* sections from
the camera at draw time, only stepping into a neighbour through a face
pair that is actually connected. Anything unreached is not drawn.

Underground this is worth **up to 70×** - in one measured cave, 283
sections drawn falls to 4.

### Proving a culler is correct

A culler has one failure mode that matters: dropping something the camera
*could* have seen. The test is a **byte-identity check** - render a frame
with culling on and with it off, and require the two images to be
identical byte for byte. If culling removed anything visible, a pixel
differs and the check fails.

---

## Part 11: How all of it is kept honest

This is the part that distinguishes the project, and it is worth being
able to talk about.

- **Nine test binaries** under ctest, each failing on its own line
- **A differential fuzz** comparing greedy against naive face-for-face
  over 180 cases
- **A CI performance gate**: if the greedy ratio drops below 4.5×, the
  build fails. A performance win that is not gated is a win that quietly
  regresses.
- **Byte-identity gates**: `--bench` runs three times and must emit
  identical output. Both CI platforms run it, and the x86-64 Ubuntu
  runner emits the same bytes as the arm64 M4 - so "reproduces on any
  GPU" is demonstrated, not asserted.
- **`--validate`** reads meshes back off the GPU with
  `glGetBufferSubData` and checks every triangle against the voxel data
  that produced it. It reports `bad_triangles=0`.
- **TSan and ASan/UBSan** over the whole logic suite in CI.

---

## Part 12: Your résumé bullets, line by line

Both bullets, decomposed. Read this before an interview.

> **Built a C++20/OpenGL engine streaming a 40-million-voxel world at
> 229 FPS in 188 MB of RAM, using greedy meshing to cut triangles 18x and
> shrink GPU memory from 48 to 12.5 MB with a CI gate blocking
> regressions.**

| Claim | What it means | Status |
|---|---|---|
| 40-million-voxel | 625 chunks × 65,536 = 40,960,000 | **exact** |
| 229 FPS | frame rate at render distance 12 | repo now says 218; run-to-run spread |
| 188 MB RAM | process RSS | that is the radius-**8** figure (189 MB); radius 12 is 289 MB |
| **18× triangles** | greedy vs naive | **repo now says 5.33×** - see below |
| **48 → 12.5 MB** | GPU mesh memory | **repo now says 126.7 → 10.9 MB** |
| CI gate | build fails below a threshold | true, gate is at 4.5× |

> **Designed a 9-thread chunk pipeline meshing terrain at 2,200 chunks/sec
> with 8.4x parallel efficiency and occlusion culling drawing up to 70x
> less hidden geometry, verified with thread and address sanitizers in CI.**

| Claim | Status |
|---|---|
| 9-thread pipeline | **exact** - `hardware_concurrency() - 1` on a 10-core M4 |
| 2,200 chunks/sec | **holds** - measured 2,226 |
| 8.4× parallel efficiency | real range is 6.3-8.5×; 8.4 is the top of it |
| up to 70× occlusion | **exact** - `occl_cave=70.75` |
| TSan + ASan in CI | **true** |

### The two that moved, and how to answer

**Why does your résumé say 18× and the repo say 5.33×?**

Because the baseline was wrong, and I fixed it.

The naive mesher used to treat "outside this chunk" as air, so it emitted
a face at every chunk boundary. Those faces are buried between two solid
chunks - no camera can reach them. They inflated the naive count
enormously while barely affecting the greedy count, and the ratio
inherited the difference. On a contiguous chunk, 79.4% of naive quads and
32.0% of greedy quads were sealed against a solid neighbour.

Cross-chunk face culling passes each mesher job the boundary layers of
its four neighbours, so those faces are never generated by either mesher.
The ratio fell from 18.1× to **5.33×**, and the whole-world chain from
369.6 MB to 126.7 MB naive.

**The old number was not a lie - it measured per-chunk merge efficiency
correctly. It counted geometry no camera could reach.** 5.33× survives
the question "merged relative to what?"; 18× does not.

This is a good answer, not a bad one. It is the story of finding your own
inflated baseline and correcting it, and the repo's README documents the
change in your own words at the line that says "Removing them cut the
contiguous ratio from 18.1x to 5.3x."

**Why 48 → 12.5 MB versus 126.7 → 10.9 MB?**

Two separate corrections. First, 48 MB was already the *post-greedy*
figure, so the old span claimed three optimizations over a range that
only contained two. The true chain starts at the naive baseline. Second,
the same cross-chunk culling moved every number in it.

---

## Part 13: Questions you should expect

**"Why not use an octree or sparse voxel structure?"**
A flat array is O(1) to index with no pointer chasing, and the meshing
pass touches every voxel anyway - so a sparse structure saves memory on
air but costs on the traversal that dominates. 64 KB per chunk is cheap.
Worth adding: it is a defensible default, not the only answer, and a
world with far more empty space would flip it.

**"What is the actual bottleneck?"**
Depends where you stand and what you measure. On the whole frame at
radius 16, post-processing is the largest single pass. On the 4D
cross-section path, the terrain and shadow passes are - all of the extra
cost there is geometry. Chunk loading is bound by worker throughput, and
about two thirds of a 4D chunk rebuild is terrain noise
generation, not meshing - 3.5 ms of 5.1 ms, measured.

**"How do you know greedy meshing is correct?"**
The differential fuzz: 180 cases, every quad from both meshers decomposed
back to unit faces and compared as sets. And `--validate`, which reads
the meshes back off the GPU and checks each triangle against the voxel
data.

**"Why is the lock-free queue not used?"**
Because profiling said it was not needed at ~1 ms job granularity. It is
in the repo, TSan-clean and 2-5× faster under contention, and the mutex
pool shipped anyway.

**"What would you do next?"**
Terrain generation, which is about two thirds of a 4D chunk rebuild and
the thing that limits how fast the world responds to a slice rotation.
The 4D generator is roughly eight times more expensive per chunk than
the 3D one, so there is room. Not the mesher - I measured the remaining
merge headroom at about 1%.

One piece of that is already done: every heightfield in the 4D
generator reads a 3D slice of the 4D field with one axis pinned at
zero, where half the sixteen hypercube corners are computed and then
multiplied away by an interpolation weight of exactly zero. Skipping
them made `height_at_4d` 1.94x faster with bit-identical output.

**"What went wrong?"**
Have one ready. Good candidates: the cross-chunk culling that revealed
the inflated baseline; an unclosed `if(APPLE)` in CMake that left Linux
CI running six tests of eight while reporting 100% passed; a NaN cast to
`int` that UBSan caught the moment those tests actually ran.

---

## Where to read more

| | |
| --- | --- |
| [performance.md](performance.md) | every measurement, with the command that produces it |
| [design.md](design.md) | architecture and layering |
| [cross-section.md](cross-section.md) | the four-dimensional work built on top of this |
