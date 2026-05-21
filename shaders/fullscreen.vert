#version 410 core

// Attributeless fullscreen triangle. Shared by every post-process pass.
out vec2 v_uv;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
