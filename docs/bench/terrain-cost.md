# Where 4D terrain generation actually spends its time

Measured 2026-09-09, M4, `-O2`, medians of 7 rounds with the
configurations alternated inside each round and two warm-up passes
discarded. Chunk fill only - no meshing, no GL.

**Result: the density field is roughly three quarters of it, and the
heightfield is three percent.**

| pass | ms/chunk | share |
| :--- | ---: | ---: |
| full `fill_chunk` | 1.87 | 100% |
| height + density + block writing | 1.47 | 79% |
| caves | 0.37 | 20% |
| structures | 0.02 | 1% |
| — of which `height_at_4d` | 0.053 | **3%** |

So density plus the block writing is about 1.42 ms, **76%**.

## Why

Per column, the density field runs

```cpp
for (int y = lo; y <= hi; ++y)            // up to 2 * 20 + 1 = 41 values
    density_.fbm(x4, y * 5.5f, z4, fw, 4, 0.030f);   // 4 octaves
```

Up to **164 noise samples per column**, so about **42,000 per chunk**.
The heightfield runs fourteen per column, 3,584 per chunk. Density does
roughly twelve times the work, which is the whole story.

## What this corrects

Two figures quoted earlier in the repo were wrong, both from reading
`--bench-4d`'s summary rather than measuring directly:

- "terrain is 77% of a chunk rebuild" - measured, it is 68%
- the claim that `height_at_4d` was the thing worth optimising

`--bench-4d` reports `terrain.fill_chunk` at 4.02 ms/chunk where a clean
load reports 0.49 ms for the 3D generator and this bench measures 1.87 ms
for the 4D one. The 4D generator genuinely costs about eight times the 3D
one per chunk; the rest of that gap was the bench's own accounting.

The `sample_xyw` optimisation that prompted this measurement is real -
`height_at_4d` is exactly 1.94x faster with a bit-identical checksum -
but it lands on 3% of chunk fill, so it is worth about **1% of a chunk
rebuild**. An engine-level A/B cannot resolve that: the three interleaved
rounds that appeared to show 13% were noise inside a 32% run-to-run
spread, and the commit that reported them overstated the result.

## Where the next win is, if there is one

The density loop, not the heightfield and not the mesher (whose
remaining merge headroom measures about 1%, see
[merge-headroom.md](merge-headroom.md)).

For a fixed column, `x4`, `z4` and `fw` are constant and only `y` moves,
so every sample down a column shares its x, z and w lattice coordinates
and their interpolation weights. At the lowest octave the y coordinate
advances 5.5 × 0.030 = 0.165 per block, so about six consecutive samples
fall in the same lattice cell and hash the same sixteen corners.

Caching those hashes down a column would be exact - same arithmetic,
same order, only the integer mixing skipped. Regrouping the gradient dot
product to hoist the x/z/w terms would **not** be: floating-point
addition does not associate, and this repo gates a byte-identical world
on both CI architectures. That distinction is the whole design
constraint on any future work here.

## Reproducing

The harness is not shipped - it toggles `set_caves_enabled` and
`set_structures_enabled` around `fill_chunk` and takes medians. Roughly:

```cpp
world::TerrainGen4D g(1337);
once(g, N); once(g, N);                     // warm up, discard
for (int r = 0; r < 7; ++r) {
    g.set_caves_enabled(true);  g.set_structures_enabled(true);  full.push_back(once(g,N));
    g.set_caves_enabled(false); g.set_structures_enabled(true);  no_caves.push_back(once(g,N));
    g.set_caves_enabled(true);  g.set_structures_enabled(false); no_str.push_back(once(g,N));
    g.set_caves_enabled(false); g.set_structures_enabled(false); neither.push_back(once(g,N));
}
```

where `once` times `N = 40` calls to `fill_chunk` at slice zero and
returns milliseconds per chunk. Warm-up matters more than usual here: the
first measured round reported caves at 43.7% against a settled 20%.
