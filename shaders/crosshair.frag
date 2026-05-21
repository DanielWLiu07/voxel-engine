#version 410 core

in  vec2 v_ndc;
out vec4 frag_color;

uniform vec2  u_screen_size;  // framebuffer pixels
uniform float u_arm_px;       // half-length of each plus arm, in pixels
uniform float u_stroke_px;    // half-thickness of the stroke, in pixels

void main() {
    // Pixel offset from screen center, regardless of resolution.
    vec2 px = v_ndc * 0.5 * u_screen_size;
    vec2 ap = abs(px);

    // Plus mask: inside a horizontal bar OR a vertical bar.
    float horiz = step(ap.x, u_arm_px) * step(ap.y, u_stroke_px);
    float vert  = step(ap.y, u_arm_px) * step(ap.x, u_stroke_px);
    float plus  = max(horiz, vert);
    if (plus < 0.5) discard;

    // 1px white core at the very center, black outline elsewhere.
    float core = step(max(ap.x, ap.y), 1.0);
    vec3 rgb = mix(vec3(0.0), vec3(1.0), core);
    frag_color = vec4(rgb, 1.0);
}
