#version 410 core

// A silhouette. Birds at this distance are shape, not colour, and a dark
// mark against a bright sky is the whole read.
in float v_alpha;
out vec4 frag_color;

void main() {
    frag_color = vec4(vec3(0.12, 0.13, 0.16), v_alpha);
}
