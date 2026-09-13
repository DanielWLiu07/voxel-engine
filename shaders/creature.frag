#version 410 core

// Lit by the same sun the terrain is, so a creature sits in the scene
// rather than on top of it, and fogged by the same curve so it fades
// into the distance with everything else.
in vec3 v_normal;
in vec3 v_world;

uniform vec3  u_light_dir;
uniform vec3  u_light_color;
uniform vec3  u_ambient_color;
uniform vec3  u_camera_pos;
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;
uniform vec3  u_tint;

out vec4 frag_color;

void main() {
    vec3  N = normalize(v_normal);
    float d = max(dot(N, normalize(u_light_dir)), 0.0);
    vec3  lit = u_tint * (u_ambient_color + u_light_color * d);

    float dist = length(v_world - u_camera_pos);
    float f = clamp((dist - u_fog_start) / max(u_fog_end - u_fog_start, 1e-4),
                    0.0, 1.0);
    frag_color = vec4(mix(lit, u_fog_color, f), 1.0);
}
