#version 410 core

// Water plane: takes a flat vec3 position (the plane sits at u_sea_level
// before displacement) and adds a small sine-based wave bump in Y based
// on world (x, z) and time. Rendered as a grid of quads so per-vertex
// displacement actually produces visible ripples.
//
// The model matrix translates the plane to the player's xz each frame
// (lets a finite plane follow an infinite streaming world). The wave
// function reads the post-translation world coords so the ripples stay
// anchored to the world — they don't surf along with the camera.
layout(location = 0) in vec3 a_position;

uniform mat4  u_model;
uniform mat4  u_view;
uniform mat4  u_proj;
uniform float u_time;
uniform float u_sea_level;

out vec3 v_world_pos;

float wave_height(vec2 xz, float t) {
    float w1 = sin(xz.x * 0.18 + t * 1.30) * 0.18;
    float w2 = sin(xz.y * 0.22 + t * 0.95 + 1.7) * 0.14;
    float w3 = sin((xz.x + xz.y) * 0.10 + t * 0.55) * 0.10;
    return w1 + w2 + w3;
}

void main() {
    vec3 world = (u_model * vec4(a_position, 1.0)).xyz;
    world.y = u_sea_level + wave_height(world.xz, u_time);
    v_world_pos = world;
    gl_Position = u_proj * u_view * vec4(world, 1.0);
}
