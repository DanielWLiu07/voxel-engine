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

void main() {
    // The plane is flat-ish (small wave bumps), so the analytic normal is
    // basically up. Good enough for a stylized look; reconstructing it
    // from derivatives would just add ALU for no visible win at our
    // camera angles.
    vec3 N = vec3(0.0, 1.0, 0.0);

    vec3 V = normalize(u_camera_pos - v_world_pos);
    vec3 L = normalize(u_sun_dir);

    // Fresnel-ish edge fade: glancing angles (low view dot normal) look
    // brighter and more sky-toned; top-down looks deep. Schlick approx.
    float ndotv = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = pow(1.0 - ndotv, 4.0);

    vec3 base = mix(u_deep_color, u_shallow_color, fresnel);

    // Cheap specular highlight from the sun.
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(R, V), 0.0), 64.0);
    base += u_sun_color * spec * 0.35;

    // Same fog math as the terrain shader so water dissolves into the
    // distance at the same rate as the ground does.
    float d = length(v_world_pos - u_camera_pos);
    float f = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1e-4), 0.0, 1.0);
    vec3 final = mix(base, u_fog_color, f);

    // Alpha boosted at edges so the rim of the water blends into terrain
    // a little more naturally instead of cutting sharply.
    float alpha = mix(u_alpha, 1.0, fresnel * 0.4);
    frag_color = vec4(final, alpha);
}
