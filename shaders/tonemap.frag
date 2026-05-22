#version 410 core

in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float     u_exposure;
uniform float     u_bloom_intensity;

// Narkowicz 2015 ACES approximation. Compresses highlights cleanly but
// desaturates — we lift saturation back up after the curve.
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Boost saturation around the luma axis. amount=0 is identity.
vec3 saturate_boost(vec3 c, float amount) {
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(luma), c, 1.0 + amount);
}

// Subtle vignette darkens the corners — helps the eye focus center-frame.
float vignette(vec2 uv) {
    vec2 q = uv - 0.5;
    float r = dot(q, q);
    return 1.0 - r * 0.5;
}

void main() {
    vec3 scene = texture(u_scene, v_uv).rgb * u_exposure;
    vec3 bloom = texture(u_bloom, v_uv).rgb * u_bloom_intensity;
    vec3 hdr = scene + bloom;

    vec3 ldr = aces(hdr);
    ldr = saturate_boost(ldr, 0.22);              // recover ACES's loss
    ldr = pow(ldr, vec3(0.95));                   // subtle contrast lift
    ldr *= vignette(v_uv);
    ldr = pow(ldr, vec3(1.0 / 2.2));              // gamma to display

    frag_color = vec4(ldr, 1.0);
}
