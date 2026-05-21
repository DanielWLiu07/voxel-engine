#version 410 core

in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float     u_exposure;
uniform float     u_bloom_intensity;

// Narkowicz 2015 ACES approximation. Keeps highlights from clipping flat.
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 scene = texture(u_scene, v_uv).rgb * u_exposure;
    vec3 bloom = texture(u_bloom, v_uv).rgb * u_bloom_intensity;
    vec3 hdr = scene + bloom;
    vec3 ldr = aces(hdr);
    // Gamma 2.2 inverse for monitor display.
    ldr = pow(ldr, vec3(1.0 / 2.2));
    frag_color = vec4(ldr, 1.0);
}
