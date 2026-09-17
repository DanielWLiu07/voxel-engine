#version 410 core

// Two triangles are two triangles unless the fragment stage does something
// about it. The first version drew exactly what the vertices described -
// hard-cornered wedges meeting at a point - and it read as a paper plane
// rather than as anything alive.
//
// So the wing is CUT here rather than built in the vertex stage: the two
// varyings give a coordinate inside the wing, and the outer corners are
// rounded off with alpha. A rounded wing costs no extra vertices.

in vec3  v_tint;
in float v_alpha;
in float v_wing;   // 0 at the body, 1 at the outer edge
in float v_fore;   // +1 leading corner, -1 trailing

out vec4 frag_color;

void main() {
    // Round the outer corners. Squashing the fore axis makes the wing
    // broader than it is long, which is the butterfly proportion.
    float d = length(vec2(v_wing, v_fore * 0.62));
    float cut = 1.0 - smoothstep(0.78, 1.04, d);
    if (cut <= 0.01) discard;

    // Darker toward the body, and a dark margin just inside the edge -
    // most butterflies have one and it is what the eye reads as "wing"
    // instead of "triangle".
    vec3 col = mix(v_tint * 0.55, v_tint, smoothstep(0.0, 0.55, v_wing));
    float margin = smoothstep(0.62, 0.88, v_wing) * (1.0 - smoothstep(0.88, 1.0, v_wing));
    col = mix(col, col * 0.30, margin * 0.85);

    // The body itself: a dark spine where the two wings meet.
    col = mix(vec3(0.07, 0.06, 0.09), col, smoothstep(0.0, 0.22, v_wing));

    frag_color = vec4(col, v_alpha * cut);
}
