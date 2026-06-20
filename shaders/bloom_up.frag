#version 410 core

// Dual-filter (Kawase) upsample with a 3x3 tent. Runs back up the pyramid
// from the smallest mip; each step is composited additively (GL_ONE/GL_ONE)
// onto the next-larger mip, so every level contributes a successively wider
// halo. This layered accumulation is what gives the bloom its soft falloff.
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_source;

void main() {
    vec2 hp = 0.5 / vec2(textureSize(u_source, 0));  // half-texel of source
    vec3 s = texture(u_source, v_uv + vec2(-hp.x * 2.0, 0.0)).rgb;
    s += texture(u_source, v_uv + vec2(-hp.x,  hp.y)).rgb * 2.0;
    s += texture(u_source, v_uv + vec2( 0.0,   hp.y * 2.0)).rgb;
    s += texture(u_source, v_uv + vec2( hp.x,  hp.y)).rgb * 2.0;
    s += texture(u_source, v_uv + vec2( hp.x * 2.0, 0.0)).rgb;
    s += texture(u_source, v_uv + vec2( hp.x, -hp.y)).rgb * 2.0;
    s += texture(u_source, v_uv + vec2( 0.0,  -hp.y * 2.0)).rgb;
    s += texture(u_source, v_uv + vec2(-hp.x, -hp.y)).rgb * 2.0;
    frag_color = vec4(s / 12.0, 1.0);
}
