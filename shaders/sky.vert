#version 410 core

// Fullscreen triangle covering the framebuffer with no vertex buffer
// needed: gl_VertexID generates 3 vertices via bitmasks. The triangle
// extends past the screen so its interior fills the whole viewport.
out vec3 v_view_dir;

uniform mat4 u_inv_view_proj;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    gl_Position = vec4(ndc, 1.0, 1.0);

    // Reconstruct world-space direction by un-projecting the far-plane
    // point at this NDC coord. u_inv_view_proj is inverse(proj * view)
    // with translation REMOVED so we get a direction, not a position.
    vec4 world = u_inv_view_proj * vec4(ndc, 1.0, 1.0);
    v_view_dir = world.xyz / world.w;
}
