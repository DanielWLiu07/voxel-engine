#version 410 core

// Packed 12-byte chunk vertex (see gfx::VertexPacked): integer
// attributes, decoded here. Attribute 0 = x, z, normal index, ao level;
// attribute 1 = y, u, v.
layout(location = 0) in uvec4 a_xzna;
layout(location = 1) in uvec3 a_yuv;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_proj;

void main() {
    vec3 a_position = vec3(float(a_xzna.x), float(a_yuv.x), float(a_xzna.y));
    gl_Position = u_proj * u_view * u_model * vec4(a_position, 1.0);
}
