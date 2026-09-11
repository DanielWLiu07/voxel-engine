#version 410 core

// Snow arrives as points and wants to be round; rain arrives as lines and
// has no gl_PointCoord to shape. Rasterizing a line leaves gl_PointCoord
// undefined, so the disc is only applied when a point was drawn.
in float v_alpha;
in vec3  v_tint;

uniform int u_snow;

out vec4 frag_color;

void main() {
    float a = v_alpha;
    if (u_snow == 1) {
        float r = length(gl_PointCoord - vec2(0.5)) * 2.0;
        if (r > 1.0) discard;
        a *= pow(1.0 - r, 1.5);
    }
    frag_color = vec4(v_tint, a);
}
