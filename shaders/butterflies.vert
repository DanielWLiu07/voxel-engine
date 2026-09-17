#version 410 core

// Butterflies: the daytime answer to the fireflies.
//
// The motes go to pale dust by day on purpose - a firefly in sunlight is
// nothing - which left the daylit world with no small moving life in it at
// all. These fill that hour, and they are the same trick one step further:
// six vertices a butterfly instead of one, so each gets two wings that
// actually beat.
//
// gl_VertexID / 6 is which butterfly, gl_VertexID % 6 is which corner of
// which wing. No buffer, one draw call, nothing on the CPU.

uniform mat4  u_view;
uniform mat4  u_proj;
uniform vec3  u_camera_pos;
uniform float u_time;
uniform vec3  u_box;
uniform float u_strength;
uniform float u_day;        // 1 by day, 0 at night

out vec3  v_tint;
out float v_alpha;
out float v_wing;           // 0 at the body, 1 at the wingtip
out float v_fore;           // +1 leading corner, -1 trailing

float h1(float n) { return fract(sin(n * 12.9898) * 43758.5453123); }
vec3  h3(float n) { return vec3(h1(n), h1(n + 17.13), h1(n + 41.71)); }

void main() {
    int   bid = gl_VertexID / 6;
    int   cor = gl_VertexID % 6;
    float id  = float(bid);
    vec3  r   = h3(id);

    // A wandering path, not a straight line: two slow circles at different
    // rates and radii, which never repeats visibly and never leaves the
    // slab.
    float sp = 0.22 + r.x * 0.25;
    vec3 p = r * (u_box * 2.0);
    p.x += sin(u_time * sp + r.y * 6.2831) * 3.4
         + sin(u_time * sp * 2.3 + r.z * 6.2831) * 1.1;
    p.z += cos(u_time * sp * 0.87 + r.z * 6.2831) * 3.4
         + cos(u_time * sp * 1.9 + r.x * 6.2831) * 1.1;
    // Bobbing, because a butterfly does not hold an altitude.
    p.y += sin(u_time * (1.1 + r.y * 0.9) + r.x * 6.2831) * 0.75;

    vec3 center = u_camera_pos + vec3(0.0, -1.5, 0.0);
    vec3 rel    = mod(p - center + u_box, u_box * 2.0) - u_box;
    vec3 world  = center + rel;

    // Heading comes from the derivative of the path, so the body points
    // where it is actually going rather than at a stored angle.
    vec3 fwd = normalize(vec3(cos(u_time * sp + r.y * 6.2831),
                              0.18,
                              -sin(u_time * sp * 0.87 + r.z * 6.2831)));
    vec3 up    = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(fwd, up));

    // The beat. Wings hinge about the body's forward axis, so the pair
    // opens and closes rather than sliding sideways, and each butterfly
    // keeps its own rate.
    float beat = sin(u_time * (11.0 + r.z * 6.0) + r.x * 6.2831);
    float flap = beat * 1.15;

    // Six vertices: body, tip, tail for the left wing, then the right.
    // Corner 0/3 sit on the body so the two triangles share a spine.
    float side = (cor < 3) ? 1.0 : -1.0;
    vec3 wing  = right * side * cos(flap) + up * abs(sin(flap));

    const float kSpan = 0.15;   // metres, tip to body
    const float kLen  = 0.115;

    vec3 offset = vec3(0.0);
    int  k = cor % 3;
    v_wing = (k == 0) ? 0.0 : 1.0;
    v_fore = (k == 0) ? 0.0 : ((k == 1) ? 1.0 : -1.0);
    if (k == 1) offset = wing * kSpan + fwd * kLen;
    if (k == 2) offset = wing * kSpan - fwd * kLen;

    vec4 view   = u_view * vec4(world + offset, 1.0);
    gl_Position = u_proj * view;

    float dist = max(length(view.xyz), 0.001);
    vec3 t_edge = abs(rel) / u_box;
    float edge = 1.0 - smoothstep(0.55, 0.95,
                                 max(t_edge.x, max(t_edge.y, t_edge.z)));

    vec3 palette[4] = vec3[4](vec3(0.95, 0.52, 0.12),   // monarch
                              vec3(0.25, 0.45, 0.92),   // morpho
                              vec3(0.95, 0.85, 0.25),   // sulphur
                              vec3(0.88, 0.88, 0.92));  // cabbage white
    v_tint  = palette[int(r.y * 3.999)];
    v_alpha = edge * u_strength * u_day
            * smoothstep(0.5, 2.5, dist)          // not on the lens
            * (1.0 - smoothstep(26.0, 42.0, dist));
}
