#version 410 core

// A soft round sprite. gl_PointCoord is the unit square of the point, so
// the distance from its centre gives a disc without any texture to load.
in float v_alpha;
in vec3  v_tint;

out vec4 frag_color;

void main() {
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;
    if (r > 1.0) discard;
    // Squared falloff, plus a tight core so a firefly has a bright centre
    // for the bloom pass to catch.
    float body = pow(1.0 - r, 2.0);
    float core = pow(1.0 - r, 12.0);
    float a = v_alpha * (body * 0.6 + core * 0.9);
    // Additive: written into the HDR target before tonemapping, so the
    // bright core blooms.
    frag_color = vec4(v_tint * a, a);
}
