#version 410 core

// Birds, four vertices each, drawn as two line segments forming a V.
//
// Same procedural plan as the motes and the weather: no vertex buffer, no
// CPU state, every position from gl_VertexID and the clock. A flock is one
// draw call and nothing to simulate.
//
// The V is built in the CAMERA's plane rather than the world's, so a bird
// never turns edge-on and vanish - at this size that reads as a flicker
// rather than as a bird banking.

uniform mat4  u_view;
uniform mat4  u_proj;
uniform vec3  u_camera_pos;
uniform float u_time;
uniform float u_day;        // 1 - star_fade: birds fly by day
uniform float u_strength;

out float v_alpha;

float h1(float n) { return fract(sin(n * 12.9898) * 43758.5453123); }
vec3  h3(float n) { return vec3(h1(n), h1(n + 17.13), h1(n + 41.71)); }

void main() {
    int   vid  = gl_VertexID;
    float bird = float(vid >> 2);
    int   part = vid & 3;          // 0 left tip, 1-2 body, 3 right tip
    vec3  r    = h3(bird);

    // Flocks, not a uniform scatter. Birds share a circling centre with
    // the others in their flock and keep their own offset inside it, which
    // is what makes them read as together rather than as sixty specks.
    float flock = floor(bird / 12.0);
    vec3  fr    = h3(flock + 101.0);

    float orbit_r = 26.0 + fr.x * 30.0;
    float orbit_s = 0.06 + fr.y * 0.05;
    float ang     = u_time * orbit_s + fr.z * 6.2831;
    vec3  centre  = u_camera_pos
                  + vec3(cos(ang) * orbit_r,
                         14.0 + fr.y * 12.0,
                         sin(ang) * orbit_r);

    // Position within the flock, and a slow bob so they are not a rigid
    // lattice being carried around.
    vec3 within = (r - 0.5) * vec3(14.0, 5.0, 14.0);
    within.y += sin(u_time * (0.7 + r.x) + r.y * 6.2831) * 0.8;
    vec3 world = centre + within;

    // The V, billboarded. Wings beat on each bird's own clock.
    vec3 right = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 up    = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);

    float dist  = max(length(world - u_camera_pos), 0.001);
    // ~1 m of wingspan. The first pass used 0.3 m and a bird was a
    // one-pixel tick at any sane distance - present, but not a bird.
    float span  = 0.45 + 0.18 * r.z;
    float flap  = sin(u_time * (7.0 + r.x * 4.0) + r.y * 6.2831);
    float rise  = span * (0.25 + 0.45 * flap);

    vec3 offset = vec3(0.0);
    if (part == 0) offset = -right * span + up * rise;
    if (part == 3) offset =  right * span + up * rise;

    vec4 view = u_view * vec4(world + offset, 1.0);
    gl_Position = u_proj * view;

    // Fade out where they would be a single aliased pixel, and at night.
    float far_fade = 1.0 - smoothstep(70.0, 110.0, dist);
    v_alpha = u_strength * u_day * far_fade * 0.9;
}
