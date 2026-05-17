#version 410 core

in vec3 v_normal_ws;
in vec2 v_uv;

uniform sampler2D u_albedo;
uniform vec3 u_light_dir;  // points TOWARDS the light, normalized

out vec4 frag_color;

void main() {
    vec3 N = normalize(v_normal_ws);
    vec3 L = normalize(u_light_dir);

    float diffuse = max(dot(N, L), 0.0);
    float ambient = 0.25;
    float lighting = ambient + (1.0 - ambient) * diffuse;

    vec3 albedo = texture(u_albedo, v_uv).rgb;
    frag_color = vec4(albedo * lighting, 1.0);
}
