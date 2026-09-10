#version 410 core

// Drifting motes: fireflies at night, dust in daylight.
//
// No vertex buffer. gl_VertexID is the particle's seed and its whole
// position is computed here, so the CPU never touches one - there is no
// array to update, no spawn list, and no state to get out of sync with a
// reloaded world. It is also what makes them safe for captures: the
// position is a pure function of the seed and u_time, and scripted runs
// already pin u_time.
//
// The field is infinite without respawning anything. Each particle owns a
// fixed point in a repeating box; the box is re-centred on the camera
// every frame with a mod, so walking forward wraps the ones behind you
// around to the front. They fade out near the box edge, which is what
// hides the wrap.

uniform mat4  u_view;
uniform mat4  u_proj;
uniform vec3  u_camera_pos;
uniform float u_time;
uniform float u_night;       // LightingFrame::star_fade, 0 day .. 1 night
uniform vec3  u_box;         // half-extents of the slab around the camera
uniform float u_viewport_h;
uniform float u_strength;

out float v_alpha;
out vec3  v_tint;

float h1(float n) { return fract(sin(n * 12.9898) * 43758.5453123); }
vec3  h3(float n) { return vec3(h1(n), h1(n + 17.13), h1(n + 41.71)); }

void main() {
    float id = float(gl_VertexID);
    vec3  r  = h3(id);

    // Slow wander, with a bob that is faster than the drift so it reads as
    // something alive rather than something falling.
    float sp = 0.13 + r.x * 0.22;
    vec3 drift = vec3(sin(u_time * sp + r.y * 6.2831) * 2.5,
                      sin(u_time * (0.31 + r.z * 0.4) + r.x * 6.2831) * 1.1,
                      cos(u_time * sp * 0.9 + r.z * 6.2831) * 2.5);

    // A SLAB, not a cube. Fireflies belong in the air the player is
    // standing in - a cube puts most of them overhead, where they read as
    // stray pixels against the sky rather than as anything alive.
    // Centred BELOW the eye, not on it. A slab centred on the camera puts
    // half the swarm above the horizon, where it reads as noise against
    // the sky rather than as anything living in the world.
    vec3 center = u_camera_pos - vec3(0.0, 3.0, 0.0);
    vec3 base = r * (u_box * 2.0) + drift;
    vec3 rel  = mod(base - center + u_box, u_box * 2.0) - u_box;
    vec3 world = center + rel;

    vec4 view = u_view * vec4(world, 1.0);
    gl_Position = u_proj * view;

    float dist = max(length(view.xyz), 0.001);

    // Fade at the box edge so the wrap is invisible, and in the first few
    // metres so one does not park on the camera lens.
    vec3 t_edge = abs(rel) / u_box;
    float edge = 1.0 - smoothstep(0.55, 0.95,
                                  max(t_edge.x, max(t_edge.y, t_edge.z)));
    float near_fade = smoothstep(0.6, 3.0, dist);

    // Night is fireflies: warm, larger, and each one pulses on its own
    // clock. Day is dust: cool, small, and only visible against shadow.
    float pulse = 0.55 + 0.45 * sin(u_time * (1.7 + r.y * 1.9) + r.z * 6.2831);
    float night_a = 0.35 + pulse * 1.05;
    float day_a   = 0.13;
    // And the ones still overhead are dimmed hard, for the same reason.
    float low = 1.0 - smoothstep(0.1, 0.85, max(0.0, rel.y) / u_box.y);
    v_alpha = mix(day_a, night_a, u_night) * edge * near_fade * u_strength
            * mix(1.0, low, u_night);

    vec3 firefly = vec3(1.00, 0.82, 0.30);
    vec3 dust    = vec3(0.95, 0.95, 1.00);
    v_tint = mix(dust, firefly, u_night);

    float size_world = mix(0.05, 0.16, u_night);
    gl_PointSize = clamp(u_viewport_h * size_world / dist, 1.0, 24.0);
}
