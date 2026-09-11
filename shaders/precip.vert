#version 410 core

// Rain and snow, on the same procedural plan as the motes: no vertex
// buffer, no CPU state, every position derived from gl_VertexID and the
// clock. See motes.vert for why that is the shape this engine uses.
//
// One shader, two primitives. Rain is drawn as GL_LINES, two vertices a
// drop, and the odd vertex trails behind the even one along the fall
// direction - a streak, which is what rain looks like and what a point
// sprite cannot be. Snow is drawn as GL_POINTS, because a flake is round
// and tumbles rather than falls straight.

uniform mat4  u_view;
uniform mat4  u_proj;
uniform vec3  u_camera_pos;
uniform float u_time;
uniform vec3  u_box;        // half-extents of the column around the camera
uniform float u_viewport_h;
uniform float u_strength;   // 0..1, how hard it is coming down
uniform int   u_snow;       // 1 snow, 0 rain

out float v_alpha;
out vec3  v_tint;

float h1(float n) { return fract(sin(n * 12.9898) * 43758.5453123); }
vec3  h3(float n) { return vec3(h1(n), h1(n + 17.13), h1(n + 41.71)); }

void main() {
    // Rain issues two vertices per drop; snow one. Either way `drop` is
    // the particle and `tail` says which end of a streak this is.
    int   vid  = gl_VertexID;
    float drop = float(u_snow == 1 ? vid : vid >> 1);
    float tail = float(u_snow == 1 ? 0 : (vid & 1));

    vec3 r = h3(drop);

    // Fall speed, and the sideways drift the wind gives it. Snow is slow
    // and wanders; rain is fast and nearly vertical.
    float fall  = u_snow == 1 ? (1.4 + r.x * 0.9) : (26.0 + r.x * 10.0);
    float sway  = u_snow == 1 ? 1.8 : 0.25;
    vec3 drift = vec3(sin(u_time * (0.5 + r.y) + r.z * 6.2831) * sway,
                      0.0,
                      cos(u_time * (0.4 + r.z) + r.y * 6.2831) * sway);
    // Rain leans with the wind rather than wandering in it.
    vec3 lean = vec3(2.5, 0.0, 1.2) * float(1 - u_snow);

    vec3 base = r * (u_box * 2.0);
    base.y -= u_time * fall;                 // falling
    base += drift + lean * (r.x * 0.5 + 0.5);

    // Wrapped into a column that follows the camera, so it rains
    // everywhere without anything ever being spawned or retired.
    vec3 center = u_camera_pos + vec3(0.0, u_box.y * 0.35, 0.0);
    vec3 rel  = mod(base - center + u_box, u_box * 2.0) - u_box;
    vec3 world = center + rel;

    // The streak's trailing vertex sits back along the fall direction.
    float streak = u_snow == 1 ? 0.0 : (0.35 + r.y * 0.35);
    world += normalize(vec3(lean.x, -fall, lean.z)) * (-streak * tail);

    vec4 view = u_view * vec4(world, 1.0);
    gl_Position = u_proj * view;

    float dist = max(length(view.xyz), 0.001);
    vec3  t_edge = abs(rel) / u_box;
    float edge = 1.0 - smoothstep(0.6, 0.98,
                                  max(t_edge.x, max(t_edge.y, t_edge.z)));
    float near_fade = smoothstep(0.5, 2.5, dist);

    v_alpha = u_strength * edge * near_fade * (u_snow == 1 ? 0.95 : 0.62);
    v_tint  = u_snow == 1 ? vec3(0.94, 0.96, 1.00) : vec3(0.62, 0.72, 0.86);

    gl_PointSize = clamp(u_viewport_h * 0.085 / dist, 1.5, 14.0);
}
