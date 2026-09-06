# What is left, and what is finished

An audit on 2026-09-03, written because the honest answer to "what should
I do next here" turned out to be less obvious than expected.

## The stated scope is complete

`CLAUDE.md` locks the scope: greedy meshing, 16x256x16 chunks, frustum
culling, multithreaded chunk gen with Perlin noise, Phong lighting, block
place/break, an ImGui HUD with perf metrics. Stretch, explicitly gated on
the MVP being solid: cascaded shadow maps, vertex AO, day/night cycle.

All of it exists. All three stretch items exist. Several things beyond the
list exist - block light, a procedural sky, cross-chunk face culling,
section-graph occlusion, RLE persistence, a differential fuzzer.

That matters for planning, because it means every further change here is a
choice rather than a completion, and should be argued for on its own.

## The gap that is real: test depth

Measured rather than felt. On 2026-09-03:

    voxel   1,993 test lines over  9,313 source   ratio 0.21
    basis   5,357 test lines over 10,828 source   ratio 0.49

Less than half the sibling repo's density, on a codebase of comparable
size and age. Twelve components had no test mention at all.

Most of those twelve are GL-bound and genuinely awkward to unit test -
shaders, framebuffers, the shadow map, the atlas, the window. That is a
fair reason and it accounted for most of the gap.

It did not account for two of them:

    src/core/cli_options.cpp   244 lines, pure string -> struct
    src/render/lighting.cpp     84 lines, pure math

Both are free of GL. Both are trivially testable. Together they were the
largest untested pure logic in the repo, and writing the first line of a
test against `cli_options` immediately produced a real defect: five count
flags used `std::atoi`, so `--capture-orbit banana` parsed to zero frames
and silently opened the interactive window instead of capturing. A typo in
a capture script produced a hanging GUI rather than an error.

That is the argument for closing this gap, and it is not "coverage is
good". It is that the untested part of this repo is where the defects
were.

### Where it stands, 2026-09-05

    voxel   3,247 test lines over  9,470 source   ratio 0.34

    wc -l tests/*.cpp
    find src -name '*.cpp' -o -name '*.h' | xargs wc -l | tail -1

The basis figure is the one measured on 2026-09-03 and has not been
re-taken here; that repo is not this one's to run.

Ten components still have no test mention. Nine are the GL- and
process-bound ones the original count already excused; the tenth,
`core/key_bindings.cpp`, is 31 lines that print a table to stdout.

What the round added, and what each addition found:

- `cli_options`, in its own binary, 122 checks driven off a table of
  every value flag. Four more flags were doing what `--capture-orbit` had
  done: `--seed defualt` generated a different world, `--time-of-day
  noon` was midnight, `--radius 12O` was 12, `--pose caves` benched
  "center" and labelled the row "center". Two structural holes underneath
  them - a value flag in last position was silently ignored, and an
  unrecognised flag ran the engine on defaults.
- `terrain_gen`, eleven relationships. Corrected two stamp comments that
  had been wrong about their own trunk heights.
- `gfx/frustum`, the no-false-negatives contract its header states and
  nothing checked, plus an isolating case per plane.
- `section_visibility`, differential against an independent union-find
  flood fill across seven fill styles.
- `thread_pool` shutdown, which drained its backlog where both its header
  and its own test said it dropped it. 128.0 ms to 1.0 ms on a 512-job
  queue.
- A `static_assert` that chunks are square, which four files silently
  depended on and none of them said.

The rule that came out of it, and the more useful half of this section:
every test here was fault-injected before it was kept, and four did not
survive that. Two could not fail at all - a clamp the noise never
reaches, and plane normalization, which is invisible through the only
method `Frustum` exposes. One passed on a sample too small to contain the
thing it claimed. One looked for trees at `h + 1`, so a tree planted at
`h + 2` did not read as floating, it did not read as a tree. A test that
has never been made to fail is a claim about coverage rather than a
measurement of it.

## Ranked

**1. ~~Test `render/lighting.cpp`.~~ Done, along with the rest of the
GL-free surface.** The entry was written for one relationship - the moon
rides the sun's arc a half turn behind - and that is pinned now, with
`cli_options`, `terrain_gen`, `frustum` and `section_visibility` behind
it. The ranking stopped being the constraint once it was clear that
writing the tests was itself what found the defects.

**2. Decide about `main.cpp`.** 1,265 lines. Three extractions this week
took roughly 90 lines out of it and each was clearly worth doing on its
own terms - `FrameSampler`, `ShaderSet`, `CaptureMode` all name something
real. What is left is mostly the render loop itself, which is one
sequence, and cutting it up further risks trading a long function for
indirection that hides the order things happen in. My read is that this is
close to done and the remaining wins are small.

**3. Nothing else, unless it earns itself.** The scope is complete. The
things that would most improve the repo from here are not features: they
are the guards added this week - `check_invariance.py`,
`check_readme_flags.py` - and one more of the same shape would be worth
more than another rendering feature.

## What was considered and rejected

**More rendering features.** SSAO, volumetrics, GPU-driven culling. Each
is defensible in isolation and none of them makes the existing claims
stronger. The repo's argument is that its numbers are checked, not that it
draws the most things.

**Chasing the frame rate.** It is 5.3 ms at radius 12 on an M4, which is
3.1x inside a 60 Hz budget. Making it 4 ms changes no argument.

**A physics or gameplay layer.** Out of scope by the locked list, and it
would dilute a repo that currently has exactly one thing to say.
