#version 410 core

// Packed 12-byte chunk vertex (see gfx::VertexPacked): integer
// attributes, decoded here. Attribute 0 = x, z, normal index, ao level;
// attribute 1 = y, u, v.
layout(location = 0) in uvec4 a_xzna;
layout(location = 1) in uvec3 a_yuv;
layout(location = 2) in uint  a_block;

const vec3 kNormals[6] = vec3[6](
    vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),
    vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
// AO brightness per 0..3 occlusion level (was baked CPU-side pre-packing).
const float kAoBrightness[4] = float[4](0.45, 0.65, 0.82, 1.00);

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
    vec3 a_position = vec3(float(a_xzna.x), float(a_yuv.x), float(a_xzna.y));
    vec3 a_normal   = kNormals[a_xzna.z];
    vec2 a_uv       = vec2(float(a_yuv.y), float(a_yuv.z));
    float a_ao      = kAoBrightness[a_xzna.w];
    vec4 world = u_model * vec4(a_position, 1.0);
    v_world_pos = world.xyz;
    v_view_pos  = (u_view * world).xyz;
    v_normal_ws = mat3(u_model) * a_normal;
    v_uv = a_uv;
    v_ao = a_ao;
    v_block_id = int(a_block);
    for (int i = 0; i < 3; ++i) {
        v_light_pos[i] = u_light_vp[i] * world;
    }
    gl_Position = u_proj * u_view * world;
}
