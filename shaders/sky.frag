#version 410 core

in  vec3 v_view_dir;
out vec4 frag_color;

uniform vec3 u_sky_top;     // zenith color
uniform vec3 u_sky_horizon; // horizon haze
uniform vec3 u_sun_dir;     // points toward sun, normalized
uniform vec3 u_sun_color;

void main() {
    vec3 dir = normalize(v_view_dir);

    // Gradient by elevation: t = 0 at horizon, 1 at zenith.
    float t = clamp(dir.y * 1.2 + 0.1, 0.0, 1.0);
    t = pow(t, 0.55);
    vec3 sky = mix(u_sky_horizon, u_sky_top, t);

    // Cheap sun disc + glow toward the sun direction.
    float sun_cos = max(dot(dir, normalize(u_sun_dir)), 0.0);
    float glow    = pow(sun_cos, 32.0) * 0.35;
    float disc    = smoothstep(0.9985, 0.999, sun_cos);
    sky += u_sun_color * (glow + disc * 4.0);

    frag_color = vec4(sky, 1.0);
}
