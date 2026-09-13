# The atmosphere

Five effects make the world feel lived in: foliage that moves, fireflies
and dust, rain and snow, birds, and mist pooling in the valleys. None of
them own a vertex buffer, none hold state, and together they cost about
a tenth of the instrumented pass time.

This is how, and what looking at the renders taught that reasoning about
them did not.

## One idea, used five times

A particle system usually means an array: spawn, update, retire, upload.
That is CPU work every frame, state to keep in sync with a reloaded
world, and a buffer to manage.

None of that is here. `gl_VertexID` is the particle's seed, and the
vertex shader derives its whole position from that seed and the clock:

```glsl
float drop = float(gl_VertexID >> 2);   // which bird
vec3  r    = h3(drop);                  // its fixed random numbers
vec3  pos  = f(r, u_time);              // where it is, right now
```

What that buys:

- **One draw call each.** 9,000 motes, 7,000 raindrops, 60 birds.
- **No CPU cost at all.** There is no array to walk.
- **Nothing to get out of sync.** Load a save, travel along the fourth
  axis, rotate the slice: the field is a function, so it is simply
  correct afterwards.
- **Captures reproduce.** Position is a pure function of seed and time,
  and scripted captures already pin the clock at 100.0 for the water
  phase. Every still in the README reproduces byte for byte from the
  command in its caption, atmosphere included.

### The volume follows you, and nothing respawns

Each particle owns a fixed point in a repeating box. The box is
re-centred on the camera every frame with a `mod`, so walking forward
wraps the ones behind you around to the front:

```glsl
vec3 rel   = mod(base - centre + u_box, u_box * 2.0) - u_box;
vec3 world = centre + rel;
```

The field is therefore infinite without anything ever being spawned or
retired, and particles fade near the box edge, which is what hides the
wrap.

## What each one needed, which was not obvious

**Wind.** Leaves only - a swaying ground block tears the terrain open and
a trunk that moves with its canopy slides out of the ground. The offset
is a function of world position, so two vertices at the same point move
together and no seam opens along a greedy-merged quad. The shadow pass
applies the same offset; without it the canopy's shadow stays where the
canopy no longer is, which is more obviously wrong than no sway at all.

**Fireflies.** The first density was 3,000 across a 96 m cube - one per
295 cubic metres, invisible. And a cube centred on the camera puts half
the swarm above the horizon, where it reads as noise against the sky. The
volume is a wide flat slab centred three metres below the eye, with the
ones still overhead dimmed hard. That is the difference between stars and
fireflies in the trees.

**Weather is a lighting change first.** This is the one worth taking
away. The first version left the lighting alone and put white flakes over
a white snowfield at noon; they were invisible, and rain against a lit
blue sky was barely there either. Particles alone do not read as weather.
The sun drops 60%, shadows soften toward none, and the sky and its fog
grey over - and then the drops are visible, because there is something
for them to be visible against.

**Birds** are drawn as a V of two line segments, built in the camera's
plane rather than the world's. A bird oriented in world space turns
edge-on twice an orbit, and at this size that is a flicker, not a bank.
They fly in flocks of twelve around a shared centre, because sixty evenly
salted specks do not read as birds. The first wingspan was 0.3 m, which
is a one-pixel tick at any distance you would see one from.

**Mist needs two terms multiplied**, altitude and depth. Altitude alone
puts a wall of white at your feet; depth alone fogs a mountainside as
hard as the valley beside it. Together the low ground goes milky while
the ridges stay sharp, which is what gives a wide view layers.

**Cloud shadows multiply the sun and never the ambient.** A cloud
overhead darkens the direct light and leaves the sky light it scatters,
so shaded ground still reads blue instead of going flat grey. That one is
physics rather than taste.

## What it costs

From `--bench-frame N --pass-breakdown`, radius 12, M4:

```
shadow 2.71   sky 2.30   terrain 3.39   water 1.95   atmos 1.85   postfx 6.41
```

About a tenth of the instrumented pass time, in the same band as the
water pass. Not free.

Two notes on reading that. The breakdown brackets each pass with
`glFinish`, so it serializes work the real frame overlaps and reports an
upper bound per pass - it attributes cost between passes and is the wrong
tool for quoting a frame time. And a blunt whole-frame A/B could not
resolve this at all: three interleaved rounds with everything on gave
8.51/8.93/10.03 ms against 8.42/9.05/10.27 off, with two of three rounds
showing the version doing *more* work as faster. The machine drifted
further across the rounds than the feature moved the frame.

## Turning it off

Every effect has a scale, and 0 disables it:

```
--wind S           foliage sway
--motes S          fireflies at night, dust by day
--weather S        0 clear .. 1 downpour; omit for the natural cycle
--mist S           ground mist in the valleys
--birds S          flocks circling overhead by day
--aurora S         aurora on the night sky
--cloud-shadow S   clouds dappling the ground
```

`--weather` is an override rather than a scale, and its default is
negative meaning "not given, run the cycle". A scripted capture pins the
clock the cycle is read from, so without the override a still could only
ever show whatever `t = 100` lands on, which is a drizzle of 0.06.
