#version 410 core

// Crepuscular rays, in screen space, from the pass the bloom already runs.
//
// The effect needs one thing: a mask that is bright where the sky is and
// black where geometry stands in front of it. The march is then the
// standard one - step from this pixel toward the sun's screen position,
// accumulating the mask and decaying as it goes, so a pixel downstream of
// a treeline gets the sky the treeline did not block and darkness where
// it did.
//
// The mask is the scene above a threshold, and the threshold is the whole
// problem. Two versions of it failed in opposite directions before this
// one, and both failures are instructive:
//
// Reusing the bloom's bright-extract, which is the obvious thing to
// share, gives a threshold of 1.0 - where a highlight starts blooming.
// Nothing in the frame clears that but the sun's own disc, so the march
// smeared a few hundred pixels into a halo: measured at a mean of
// 0.097/255 against the same frame with the pass switched off.
//
// A fixed low threshold (0.42) fails the other way. It makes the ENTIRE
// sky an emitter, and since every pixel marches toward the sun through
// sky, every pixel picks up the same wash - the frame went milky white
// from corner to corner with no shafts in it at all. Crepuscular rays
// need the emitter to be mostly BLACK; what reads as a shaft is the
// contrast between a ray that reached the sun's glow and one a tree
// stopped.
//
// So the threshold is passed in RELATIVE to the sky's own brightness,
// which the CPU knows exactly because it authored the gradient. Only
// what is brighter than the ambient sky emits: the sun's glow, its disc,
// and sunlit cloud tops. That tracks the day cycle for free, where any
// fixed number is wrong at some hour - the sky's luma runs from 0.05 at
// night to 0.7 at noon.

in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;      // resolved HDR scene
uniform vec2      u_sun_uv;     // sun's screen position, [0,1]
uniform float     u_density;    // fraction of the distance to the sun to walk
uniform float     u_decay;      // per-sample falloff along the ray
uniform float     u_weight;     // overall strength
uniform float     u_threshold;  // luminance a pixel must clear to emit

// Everything the ray is allowed to pick up. A soft knee rather than a
// step: a hard cutoff makes the cloud edges that cross the threshold
// flicker between frames as they drift.
vec3 emitter(vec2 uv) {
    vec3 c = texture(u_scene, uv).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return c * smoothstep(u_threshold, u_threshold + 0.35, luma);
}

const int kSamples = 32;

void main() {
    vec2 delta = (v_uv - u_sun_uv) * (u_density / float(kSamples));

    // Jitter the first step by a per-pixel amount. Without it every pixel
    // samples the same 48 positions along its ray and the shafts come out
    // in visible rings around the sun - the march is coarse enough to
    // alias, and a quarter-step of noise costs nothing and breaks it into
    // grain the tonemap then swallows.
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) *
                         43758.5453);

    vec2  uv    = v_uv - delta * jitter;
    float illum = 1.0;
    float total = 0.0;
    vec3  acc   = vec3(0.0);
    for (int i = 0; i < kSamples; ++i) {
        uv -= delta;
        acc += emitter(uv) * illum;
        total += illum;
        illum *= u_decay;
    }
    // Normalised by the weights actually applied, not by the sample count,
    // so u_weight means "this fraction of the emitter's brightness" and
    // stays meaning that if the decay or the sample count is ever retuned.
    frag_color = vec4(acc * (u_weight / max(total, 1e-4)), 1.0);
}
