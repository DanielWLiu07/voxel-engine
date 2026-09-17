#version 410 core

in  vec3 v_world_pos;
out vec4 frag_color;

uniform vec3  u_camera_pos;
uniform vec3  u_deep_color;
uniform vec3  u_shallow_color;
uniform vec3  u_sun_dir;
uniform vec3  u_sun_color;
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;
uniform float u_alpha;
uniform float u_time;
// The sky, so the water can reflect it. Same two colours the sky shader
// builds its gradient from, plus what it needs for the night side.
uniform vec3  u_sky_top;
uniform vec3  u_sky_horizon;
uniform vec3  u_moon_dir;
uniform float u_star_fade;
uniform float u_precip;       // rain this frame, 0..1, for surface dimples
uniform float u_submerged;  // 1 when the camera is under this surface

// Analytic surface normal from the wave field's exact derivatives. The
// vertex shader displaces with the three broad swells; the normal adds two
// higher-frequency detail waves on top (normal-only - displacing them
// would need a much denser grid, but slope is what lighting sees, and
// slope = amplitude * frequency survives small amplitudes).
// Rings where drops land.
//
// A grid cell owns at most one impact, but MOST CELLS ARE EMPTY at any
// moment. The first version gave every cell a ring on every cycle and the
// lake came out looking like bubble wrap - a regular lattice of identical
// circles, which is the one thing falling rain never produces.
//
// So a second hash, seeded by the cell AND the cycle index, decides
// whether that cell fires at all, and the impact point is jittered inside
// the cell. Roughly a third of cells carry a ring at a time and each one
// lands somewhere different on the next pass.
float rain_rings(vec2 xz, float t) {
    vec2  g    = xz * 0.55;
    vec2  cell = floor(g);
    vec2  f    = fract(g) - 0.5;

    float h    = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
    float cyc  = t * 0.85 + h;
    float idx  = floor(cyc);
    float age  = fract(cyc);

    float fire = fract(sin(dot(cell + idx, vec2(39.34, 11.13))) * 24634.634);
    if (fire > 0.34) return 0.0;

    vec2  jit = vec2(fract(fire * 71.3), fract(fire * 133.7)) - 0.5;
    float r   = length(f - jit * 0.55);

    // A ring travelling outward and dying as it goes.
    return exp(-pow((r - age * 0.40) / 0.045, 2.0)) * (1.0 - age);
}

vec3 wave_normal(vec2 xz, float t) {
    float dx = 0.18 * 0.18 * cos(xz.x * 0.18 + t * 1.30)
             + 0.10 * 0.10 * cos((xz.x + xz.y) * 0.10 + t * 0.55);
    float dz = 0.14 * 0.22 * cos(xz.y * 0.22 + t * 0.95 + 1.7)
             + 0.10 * 0.10 * cos((xz.x + xz.y) * 0.10 + t * 0.55);
    // Detail ripples: small amplitude, high frequency, drifting in two
    // directions so the glints shimmer instead of marching in rows.
    dx += 0.045 * 0.90 * cos(xz.x * 0.90 + xz.y * 0.35 + t * 2.6);
    dz += 0.045 * 0.90 * cos(xz.x * 0.35 + xz.y * 0.90 - t * 2.2);
    // Rain dimples the surface. Finite differences on the ring field
    // rather than an analytic derivative: the field is already a hash and
    // two extra samples are cheaper than differentiating it by hand.
    if (u_precip > 0.02) {
        const float e = 0.06;
        float c  = rain_rings(xz, t);
        float gx = rain_rings(xz + vec2(e, 0.0), t) - c;
        float gz = rain_rings(xz + vec2(0.0, e), t) - c;
        dx += gx * 1.15 * u_precip;
        dz += gz * 1.15 * u_precip;
    }
    return normalize(vec3(-dx, 1.0, -dz));
}

// The sky along one direction, split in two because the two halves want
// different treatment when they land on water.
//
// Both are a deliberate subset of sky.frag: the elevation gradient and
// the two light discs, but no clouds, no stars, no aurora. Those are high
// frequency and the surface they would land on is moving, so they cost
// five octaves of fbm per pixel to produce shimmer that reads as noise.
// The gradient and the glow are what carry the HOUR, which is the part
// that was wrong. Duplicated rather than shared because GLSL has no
// include and the project does not add a preprocessor for one function -
// the gradient is three lines and is marked in both files.
//
// Below the horizon the ray would leave the sky dome, so it is folded
// back up: water seen at a glancing angle from a low camera reflects the
// sky just above the horizon, not the ground under it.
vec3 sky_gradient(vec3 d) {
    float elev = clamp(abs(d.y) * 1.2 + 0.1, 0.0, 1.0);
    return mix(u_sky_horizon, u_sky_top, pow(elev, 0.55));
}

// The sun's and moon's path on the water, kept apart from the gradient so
// the body colour can tint one without touching the other. Tinting both
// together is what a first version did, and since the water hue is
// (0.15, 0.42, 0.60) it multiplied the red channel of a sunset by 0.33 -
// deleting the one thing on a lake at that hour anybody would name.
vec3 sky_glitter(vec3 d) {
    // Wider than the sky's own glow (32) because the wave normals scatter
    // it across the surface anyway, and a tighter exponent breaks it into
    // pixel-sized fragments that crawl as the camera moves.
    float sun_cos = max(dot(d, normalize(u_sun_dir)), 0.0);
    vec3 g = u_sun_color * pow(sun_cos, 14.0) * 0.85;

    // The moon lays a path the same way, and at night it is the only
    // thing that does.
    if (u_star_fade > 0.001) {
        float moon_cos = max(dot(d, u_moon_dir), 0.0);
        g += vec3(0.98, 0.97, 0.90) * pow(moon_cos, 90.0) * 0.42 *
             u_star_fade;
    }
    return g;
}

void main() {
    vec3 N = wave_normal(v_world_pos.xz, u_time);
    vec3 V = normalize(u_camera_pos - v_world_pos);
    vec3 L = normalize(u_sun_dir);

    // Schlick-style edge response: glancing views read as sky-toned
    // reflection, top-down views read as the water body itself.
    float ndotv = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = pow(1.0 - ndotv, 4.0);

    // What the surface reflects, rather than a colour that was authored
    // to look like a reflection. The old shader lerped toward a fixed
    // sky-cyan dimmed by the day cycle, which is right at noon and wrong
    // at every other hour: at sunset it put a dark blue lake under a
    // peach sky, and the capture notes worked around it by telling
    // photographers to raise the sun for lake shots.
    //
    // Tinted toward the body colour rather than taken raw. Still water is
    // very close to a mirror at grazing angles, but a voxel lake with
    // three swells on it is not still, and an untinted reflection reads
    // as chrome.
    vec3 R_view = reflect(-V, N);
    // The gradient picks up the body's hue, because a lake is not a
    // mirror and an untinted reflection reads as chrome. The glitter does
    // not, because a sun path on water is the colour of the sun.
    vec3 reflected = mix(sky_gradient(R_view),
                         sky_gradient(R_view) * u_shallow_color * 2.2,
                         0.35)
                   + sky_glitter(R_view);

    vec3 base = mix(u_deep_color, reflected, fresnel);

    // Sun glints off the rippled normal. The tight exponent keeps the
    // highlight as sparkle lines along wave crests rather than a wash.
    // Sharp sparkles on top of the broad path the reflection already
    // carries. Halved when that landed - two sun terms at the old
    // strength blew the crests to white and bloom then smeared them.
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(R, V), 0.0), 96.0);
    base += u_sun_color * spec * 0.25;

    // Same fog math as the terrain shader so water dissolves into the
    // distance at the same rate as the ground does.
    float d = length(v_world_pos - u_camera_pos);
    float f = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1e-4), 0.0, 1.0);
    vec3 final = mix(base, u_fog_color, f);

    // Seen from BELOW, the same surface is a different thing. It is the
    // lid of the water, not a mirror of the sky, and leaving it as-is
    // meant a diver looking up saw an ordinary blue sky through the
    // ceiling - the one part of the underwater look that kept giving it
    // away after the fog and the sky pass were both right.
    //
    // Not a physical Snell window, which would want refraction the rest
    // of this shader does not do: just the water's own colour lit from
    // above, brightest where the view is steepest, so the surface reads
    // as a luminous ceiling that the rest of the scene fades into.
    if (u_submerged > 0.5) {
        float up = clamp(abs(V.y), 0.0, 1.0);
        vec3 lid = u_fog_color * (1.6 + 5.0 * pow(up, 3.0));
        lid += u_sun_color * pow(max(dot(reflect(-V, -N), L), 0.0), 60.0) *
               0.35 * up;
        frag_color = vec4(lid, 1.0);
        return;
    }

    // Mostly-opaque body that closes fully at glancing angles; the floor
    // should read as a hint through the surface, not compete with it.
    float alpha = mix(u_alpha, 1.0, fresnel * 0.8);
    frag_color = vec4(final, alpha);
}
