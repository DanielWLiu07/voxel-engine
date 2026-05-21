#version 410 core

in vec3  v_normal_ws;
in vec2  v_uv;
in vec3  v_world_pos;
in vec4  v_light_pos;
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

uniform sampler2DShadow u_shadow_map;
uniform float u_shadow_strength;  // 0 disables shadows (e.g. at night)

out vec4 frag_color;

float hash3(vec3 p) {
    p = fract(p * vec3(443.8975, 397.2973, 491.1871));
    p += dot(p, p.yxz + 19.19);
    return fract((p.x + p.y) * p.z);
}

// 3x3 PCF over the shadow map. Slope-scaled bias driven by surface
// normal vs light direction so flat surfaces don't shadow-acne.
float sample_shadow(vec3 N, vec3 L) {
    // Project to NDC, then map to [0, 1] for shadow sampling.
    vec3 ndc = v_light_pos.xyz / v_light_pos.w;
    vec3 sm  = ndc * 0.5 + 0.5;

    // Outside the light frustum -> no shadow contribution. The negative
    // z check catches fragments in front of the light's near plane
    // (orthographic projection makes w=1 so the divide above is benign).
    if (sm.x < 0.0 || sm.x > 1.0 ||
        sm.y < 0.0 || sm.y > 1.0 ||
        sm.z < 0.0 || sm.z > 1.0) {
        return 1.0;
    }

    float cos_a = max(dot(N, L), 0.0);
    float bias = max(0.0025 * (1.0 - cos_a), 0.0005);

    float visibility = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(u_shadow_map, 0));
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 coord = vec3(sm.xy + vec2(float(x), float(y)) * texel,
                              sm.z - bias);
            visibility += texture(u_shadow_map, coord);
        }
    }
    return visibility / 9.0;
}

void main() {
    vec3 N = normalize(v_normal_ws);
    vec3 L = normalize(u_light_dir);

    float diffuse  = max(dot(N, L), 0.0);
    float shadow   = mix(1.0, sample_shadow(N, L), u_shadow_strength);
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
