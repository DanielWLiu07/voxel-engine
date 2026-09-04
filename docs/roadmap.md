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

Measured rather than felt:

    voxel   1,993 test lines over  9,313 source   ratio 0.21
    basis   5,357 test lines over 10,828 source   ratio 0.49

Less than half the sibling repo's density, on a codebase of comparable
size and age. Twelve components have no test mention at all.

Most of those twelve are GL-bound and genuinely awkward to unit test -
shaders, framebuffers, the shadow map, the atlas, the window. That is a
fair reason and it accounts for most of the gap.

It does not account for two of them:

    src/core/cli_options.cpp   244 lines, pure string -> struct
    src/render/lighting.cpp     84 lines, pure math

Both are free of GL. Both are trivially testable. Together they are the
largest untested pure logic in the repo, and writing the first line of a
test against `cli_options` immediately produced a real defect: five count
flags used `std::atoi`, so `--capture-orbit banana` parsed to zero frames
and silently opened the interactive window instead of capturing. A typo in
a capture script produced a hanging GUI rather than an error.

That is the argument for closing this gap, and it is not "coverage is
good". It is that the untested part of this repo is where the defects
were.

## Ranked

**1. Test `render/lighting.cpp`.** 84 lines of pure math behind one entry
point, `compute_lighting(float time_of_day)`. It decides sun and moon
direction, colour, and the star fade, and the moon rides the sun's arc a
half turn behind - a relationship that is exactly the kind a test pins and
a refactor breaks silently. Cheap, and the last untested pure-logic file.

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
