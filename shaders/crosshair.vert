#version 410 core

// Fullscreen triangle, no VBO. gl_VertexID picks the 3 NDC corners
// of an oversized tri whose interior covers the viewport.
out vec2 v_ndc;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    v_ndc = ndc;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
