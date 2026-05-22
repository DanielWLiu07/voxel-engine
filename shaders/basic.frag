#version 410 core

in vec3  v_normal_ws;
in vec2  v_uv;
in vec3  v_world_pos;
in vec3  v_view_pos;
in vec4  v_light_pos[3];
in float v_ao;
flat in int v_block_id;

uniform vec3  u_light_dir;
uniform vec3  u_light_color;
uniform vec3  u_ambient_color;
uniform vec3  u_camera_pos;
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;
uniform vec3  u_palette[8];

uniform sampler2DArrayShadow u_shadow_array;
uniform float u_cascade_far[3];
uniform float u_shadow_strength;

out vec4 frag_color;

float hash3(vec3 p) {
    p = fract(p * vec3(443.8975, 397.2973, 491.1871));
    p += dot(p, p.yxz + 19.19);
    return fract((p.x + p.y) * p.z);
}

// PCF on the selected cascade. Picks the smallest cascade whose far
// plane still contains this fragment in view-space depth.
float sample_csm(vec3 N, vec3 L) {
    float vz = -v_view_pos.z;
    int cascade = 2;
    if      (vz < u_cascade_far[0]) cascade = 0;
    else if (vz < u_cascade_far[1]) cascade = 1;

    vec3 ndc = v_light_pos[cascade].xyz / v_light_pos[cascade].w;
    vec3 sm  = ndc * 0.5 + 0.5;
    if (sm.x < 0.0 || sm.x > 1.0 ||
        sm.y < 0.0 || sm.y > 1.0 ||
        sm.z < 0.0 || sm.z > 1.0) {
        return 1.0;
    }

    float cos_a = max(dot(N, L), 0.0);
    float bias = max(0.0025 * (1.0 - cos_a), 0.0005);

    float visibility = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(u_shadow_array, 0).xy);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 coord = vec4(sm.xy + vec2(float(x), float(y)) * texel,
                              float(cascade),
                              sm.z - bias);
            visibility += texture(u_shadow_array, coord);
        }
    }
    return visibility / 9.0;
}

void main() {
    vec3 N = normalize(v_normal_ws);
    vec3 L = normalize(u_light_dir);

    float diffuse  = max(dot(N, L), 0.0);
    float shadow   = mix(1.0, sample_csm(N, L), u_shadow_strength);
    vec3  lighting = u_ambient_color + u_light_color * diffuse * shadow;

    vec3 base = u_palette[clamp(v_block_id, 0, 7)];
    vec3 cell = floor(v_world_pos - N * 0.5);
    float jitter = hash3(cell) * 0.10 - 0.05;
    vec3 albedo = clamp(base * (1.0 + jitter), 0.0, 1.0);

    vec3 lit = albedo * lighting * v_ao;

    float d = length(v_world_pos - u_camera_pos);
    float f = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1e-4), 0.0, 1.0);
    vec3 final = mix(lit, u_fog_color, f);

    frag_color = vec4(final, 1.0);
}
