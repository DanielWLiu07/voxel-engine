#version 410 core

in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform float     u_threshold;

void main() {
    vec3 c = texture(u_scene, v_uv).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Soft knee: ramp from threshold to threshold+1 instead of hard step.
    float t = clamp((luma - u_threshold) / 1.0, 0.0, 1.0);
    frag_color = vec4(c * t, 1.0);
}
