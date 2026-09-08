# How much merging is left in the prism mesher

Measured 2026-09-08, M4, at HEAD. **Answer: about 1%.** The greedy merge
on the 4D cross-section path is at its optimum, and the gap between it
and the cube mesher is structural rather than a merge deficiency.

This exists because the README said otherwise, and said it specifically
enough to be worth checking:

> a merged wall has to span one uniform y range, so two cells whose
> exposed heights differ never share a quad [...] Closing that needs a
> 3D box decomposition over (two lattice axes, y) rather than the 2D
> sweep here. 1.41x is what the 2D sweep gets.

That names the y-run restriction as the reason the flat cut sits at
1.41x instead of parity. It is not the reason.

## Method

The mesher buckets faces before merging: caps by `CapKey{l, y, block,
dir, light, ao}` merged over `(i,k)`, walls by `WallKey{axis, side,
face, y0, y1, block, light}` merged over the two lattice axes the face
does not span. Both are 2D sweeps inside a bucket whose key pins the
third dimension.

The counterfactual drops that pin and re-merges in 3D:

- **caps** - regroup ignoring `l`, collect unit `(i,k,l)` cells, greedy
  box decomposition growing `l`, then `k`, then `i`
- **walls** - regroup ignoring `y0,y1`, expand each member's run into
  unit `(a,b,y)` cells, greedy box decomposition growing `y`, then `b`,
  then `a`

Both count boxes without emitting them, so the comparison is
merge-versus-merge over identical input. Run over every chunk the engine
meshes at radius 8, accumulated.

## Result

| cut | caps now | caps 3D | saved | walls now | walls 3D | saved |
|---|---:|---:|---:|---:|---:|---:|
| flat | 195,381 | 195,381 | **0.0%** | 239,506 | 237,508 | **0.8%** |
| ZW only | - | - | - | 363,082 | 360,998 | **0.6%** |
| both planes 0.45 | 505,981 | 493,800 | **2.4%** | 439,875 | 439,799 | **0.0%** |

Re-run with the light term removed from the wall grouping entirely - the
most permissive possible merge, ignoring that two faces with different
light cannot share a quad - and the wall saving stays at 0.8%. So it is
not the light term either.

## What this means

The 1.41x at a flat cut and 3.05x at a compound tilt are not slack. They
come from the cut presenting more cells (256 per chunk flat, 480 at a
hard tilt) each with its own polygon and its own edges, which is the
geometry of a hyperplane meeting a lattice at an angle, not something a
better sweep recovers.

Worth stating plainly because the opposite claim is more flattering: an
unclosed gap implies a win still available. There isn't one. The useful
version is that the merge is provably near-optimal.

## Reproducing

The counterfactual is instrumentation, not shipped code - it costs a
full re-merge per chunk and reports a number that does not change.
Patch `build_prism_mesh` in `src/world/prism_mesh.cpp` to build the two
regrouped maps described above, run the box decompositions, and print
the counts; the loops are ~40 lines each and are written out in the
commit that added this file. Then:

```
PRISM_WALL_MEASURE=1 ./build/voxel_engine --bench-frame 40 --radius 8 --slice-prisms
PRISM_WALL_MEASURE=1 ./build/voxel_engine --bench-frame 40 --radius 8 --slice-prisms \
    --slice-tilt 0.45 --slice-tilt-xw 0.45
```
