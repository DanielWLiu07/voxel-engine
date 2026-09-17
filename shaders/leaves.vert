#version 410 core

// Leaves coming off the canopy.
//
// Same plan as the motes: gl_VertexID is the seed, the whole position is a
// function of that seed and the clock, and there is no buffer to fill and
// nothing for the CPU to step. What differs is that a leaf has a net
// direction - it falls - so the wrap has to run in the direction of travel
// rather than around a static box, or leaves would pop back to the top in
// a visible sheet.

uniform mat4  u_view;
uniform mat4  u_proj;
uniform vec3  u_camera_pos;
uniform float u_time;
uniform vec3  u_box;
uniform float u_viewport_h;
uniform float u_strength;
uniform float u_wind;       // shares the foliage sway's strength
uniform float u_night;      // 0 day .. 1 night

out float v_alpha;
out vec3  v_tint;
out float v_spin;

float h1(float n) { return fract(sin(n * 12.9898) * 43758.5453123); }
vec3  h3(float n) { return vec3(h1(n), h1(n + 17.13), h1(n + 41.71)); }

void main() {
    float id = float(gl_VertexID);
    vec3  r  = h3(id);

    // Fall, then wrap. Subtracting from y before the mod is what makes the
    // column continuous: a leaf leaving the bottom re-enters at the top on
    // its own, and because every leaf has its own speed they never form a
    // layer that drops together.
    float fall = 0.55 + r.x * 0.85;
    vec3 base  = r * (u_box * 2.0);
    base.y    -= u_time * fall;

    // Sideways is a pair of sines at different rates, which is what makes
    // it read as a leaf rather than as rain: it slips sideways, stalls,
    // and slips back instead of tracking straight down.
    float sway = 1.1 + u_wind * 1.9;
    base.x += sin(u_time * (0.5 + r.y * 0.6) + r.z * 6.2831) * sway;
    base.z += cos(u_time * (0.4 + r.z * 0.7) + r.y * 6.2831) * sway;

    // Centred above the eye: leaves belong in and under the canopy, so the
    // slab sits high and the camera looks up into it.
    vec3 center = u_camera_pos + vec3(0.0, 4.0, 0.0);
    vec3 rel    = mod(base - center + u_box, u_box * 2.0) - u_box;
    vec3 world  = center + rel;

    vec4 view   = u_view * vec4(world, 1.0);
    gl_Position = u_proj * view;
    float dist  = max(length(view.xyz), 0.001);

    vec3  t_edge = abs(rel) / u_box;
    float edge = 1.0 - smoothstep(0.55, 0.95,
                                  max(t_edge.x, max(t_edge.y, t_edge.z)));
    float near_fade = smoothstep(0.7, 3.5, dist);

    // Four autumn tints plus a green, picked per leaf. Green stays in the
    // mix because a canopy sheds while it is still green.
    vec3 palette[5] = vec3[5](vec3(0.85, 0.45, 0.13), vec3(0.72, 0.24, 0.10),
                              vec3(0.90, 0.71, 0.20), vec3(0.58, 0.33, 0.12),
                              vec3(0.40, 0.62, 0.22));
    v_tint = palette[int(r.z * 4.999)];

    // Dimmer at night, but not gone: they are lit by the same moon the
    // rest of the scene is.
    v_alpha = edge * near_fade * u_strength * mix(0.95, 0.35, u_night);
    v_spin  = u_time * (1.4 + r.y * 2.2) + r.x * 6.2831;

    gl_PointSize = clamp(u_viewport_h * 0.11 / dist, 1.0, 22.0);
}
