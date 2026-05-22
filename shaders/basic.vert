#version 410 core

layout(location = 0) in vec3  a_position;
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in float a_ao;
layout(location = 4) in float a_block_id;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform mat4 u_light_vp[3];

out vec3  v_normal_ws;
out vec2  v_uv;
out vec3  v_world_pos;
out vec3  v_view_pos;
out vec4  v_light_pos[3];
out float v_ao;
flat out int v_block_id;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_pos = world.xyz;
    v_view_pos  = (u_view * world).xyz;
    v_normal_ws = mat3(u_model) * a_normal;
    v_uv = a_uv;
    v_ao = a_ao;
    v_block_id = int(a_block_id);
    for (int i = 0; i < 3; ++i) {
        v_light_pos[i] = u_light_vp[i] * world;
    }
    gl_Position = u_proj * u_view * world;
}
