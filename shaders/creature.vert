#version 410 core

// One unit cube, instanced by uniform rather than by attribute: a
// creature is two or three boxes and there are sixteen of them, so the
// draw count is trivial and a per-part uniform keeps the vertex format
// to a position and a normal.
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;

uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec3 u_origin;      // where the part sits, world space
uniform vec3 u_size;        // its extents
uniform float u_yaw;        // which way the creature faces

out vec3 v_normal;
out vec3 v_world;

void main() {
    vec3 p = a_pos * u_size;
    float c = cos(u_yaw), s = sin(u_yaw);
    vec3 rot = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    vec3 n   = vec3(a_normal.x * c - a_normal.z * s,
                    a_normal.y,
                    a_normal.x * s + a_normal.z * c);
    vec3 world = u_origin + rot;
    v_world  = world;
    v_normal = n;
    gl_Position = u_proj * u_view * vec4(world, 1.0);
}
