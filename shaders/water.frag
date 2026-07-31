#version 410 core

in  vec3 v_world_pos;
out vec4 frag_color;

uniform vec3  u_camera_pos;
uniform vec3  u_deep_color;
uniform vec3  u_shallow_color;
uniform vec3  u_sun_dir;
uniform vec3  u_sun_color;
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;
uniform float u_alpha;
uniform float u_time;

// Analytic surface normal from the wave field's exact derivatives. The
// vertex shader displaces with the three broad swells; the normal adds two
// higher-frequency detail waves on top (normal-only - displacing them
// would need a much denser grid, but slope is what lighting sees, and
// slope = amplitude * frequency survives small amplitudes).
vec3 wave_normal(vec2 xz, float t) {
    float dx = 0.18 * 0.18 * cos(xz.x * 0.18 + t * 1.30)
             + 0.10 * 0.10 * cos((xz.x + xz.y) * 0.10 + t * 0.55);
    float dz = 0.14 * 0.22 * cos(xz.y * 0.22 + t * 0.95 + 1.7)
             + 0.10 * 0.10 * cos((xz.x + xz.y) * 0.10 + t * 0.55);
    // Detail ripples: small amplitude, high frequency, drifting in two
    // directions so the glints shimmer instead of marching in rows.
    dx += 0.045 * 0.90 * cos(xz.x * 0.90 + xz.y * 0.35 + t * 2.6);
    dz += 0.045 * 0.90 * cos(xz.x * 0.35 + xz.y * 0.90 - t * 2.2);
    return normalize(vec3(-dx, 1.0, -dz));
}

void main() {
    vec3 N = wave_normal(v_world_pos.xz, u_time);
    vec3 V = normalize(u_camera_pos - v_world_pos);
    vec3 L = normalize(u_sun_dir);

    // Schlick-style edge response: glancing views read as sky-toned
    // reflection, top-down views read as the water body itself.
    float ndotv = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = pow(1.0 - ndotv, 4.0);

    vec3 base = mix(u_deep_color, u_shallow_color, fresnel);

    // Sun glints off the rippled normal. The tight exponent keeps the
    // highlight as sparkle lines along wave crests rather than a wash.
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(R, V), 0.0), 96.0);
    base += u_sun_color * spec * 0.5;

    // Same fog math as the terrain shader so water dissolves into the
    // distance at the same rate as the ground does.
    float d = length(v_world_pos - u_camera_pos);
    float f = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1e-4), 0.0, 1.0);
    vec3 final = mix(base, u_fog_color, f);

    // Mostly-opaque body that closes fully at glancing angles; the floor
    // should read as a hint through the surface, not compete with it.
    float alpha = mix(u_alpha, 1.0, fresnel * 0.8);
    frag_color = vec4(final, alpha);
}
