#version 410 core

// A leaf, not a dot. The point sprite is cut to a rounded lens shape and
// spun, so a falling leaf turns as it goes instead of sliding down as a
// disc - which is the tell that separates "leaves" from "orange snow".

in float v_alpha;
in vec3  v_tint;
in float v_spin;

out vec4 frag_color;

void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;

    // Spin the sample, then squash one axis: a circle sampled through a
    // rotation and a scale is an ellipse at an angle, which is close
    // enough to a leaf at this size and costs two trig calls.
    float s = sin(v_spin), c = cos(v_spin);
    p = mat2(c, -s, s, c) * p;
    p.x /= 0.45;

    float d = length(p);
    if (d > 1.0) discard;

    // A soft edge, and a darker midrib along the long axis.
    float body = 1.0 - smoothstep(0.65, 1.0, d);
    float rib  = 1.0 - smoothstep(0.0, 0.18, abs(p.y));
    vec3  col  = v_tint * mix(1.0, 0.72, rib * 0.8);

    frag_color = vec4(col, body * v_alpha);
}
