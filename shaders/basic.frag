#version 410 core

in vec3 v_normal_ws;
in vec2 v_uv;
in vec3 v_world_pos;

uniform sampler2D u_albedo;
uniform vec3  u_light_dir;     // points TOWARDS the light, normalized
uniform vec3  u_light_color;
uniform vec3  u_ambient_color;
uniform vec3  u_camera_pos;
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

out vec4 frag_color;

void main() {
    vec3 N = normalize(v_normal_ws);
    vec3 L = normalize(u_light_dir);

    float diffuse = max(dot(N, L), 0.0);
    vec3 lighting = u_ambient_color + u_light_color * diffuse;

    vec3 albedo = texture(u_albedo, v_uv).rgb;
    vec3 lit    = albedo * lighting;

    // Distance-based linear fog. mix(lit, fog, 1) at fog_end, mix(lit, fog, 0)
    // at fog_start. Lets terrain dissolve into the sky color near the far
    // plane and hides the chunk-loading boundary.
    float d = length(v_world_pos - u_camera_pos);
    float f = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1e-4), 0.0, 1.0);
    vec3 final = mix(lit, u_fog_color, f);

    frag_color = vec4(final, 1.0);
}
