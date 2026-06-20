#version 410 core

// Dual-filter (Kawase) downsample. One of these runs per mip step, halving
// resolution each time. Five bilinear taps centered on the source texel
// effectively average a 4x4 source neighborhood, so a few cheap passes down
// the pyramid give a wide, stable blur for a fraction of the cost of a
// fixed-resolution multi-tap Gaussian.
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_source;

void main() {
    vec2 hp = 0.5 / vec2(textureSize(u_source, 0));  // half-texel of source
    vec3 s = texture(u_source, v_uv).rgb * 4.0;
    s += texture(u_source, v_uv + vec2( hp.x,  hp.y)).rgb;
    s += texture(u_source, v_uv + vec2(-hp.x,  hp.y)).rgb;
    s += texture(u_source, v_uv + vec2( hp.x, -hp.y)).rgb;
    s += texture(u_source, v_uv + vec2(-hp.x, -hp.y)).rgb;
    frag_color = vec4(s / 8.0, 1.0);
}
