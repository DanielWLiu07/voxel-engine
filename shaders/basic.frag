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

uniform sampler2DArray       u_atlas;
uniform sampler2DArrayShadow u_shadow_array;
uniform float u_cascade_far[3];
uniform float u_shadow_strength;

out vec4 frag_color;

// One array layer per tile. v_uv runs 0..w/h across the face; GL_REPEAT
// tiles it across merged greedy quads natively, and per-layer mipmaps
// work without atlas bleed (the old packed-atlas + fract() path broke
// both mip selection and wrap filtering at tile borders).
vec3 sample_atlas(int tile_id, vec2 face_uv) {
    return texture(u_atlas, vec3(face_uv, float(tile_id))).rgb;
}

// Per-face tile picker. Grass / wood / snow have distinct top vs side
// textures sitting in row 1 of the atlas (tile ids 8, 9, 10).
int tile_for_face(int block_id, vec3 normal) {
    bool top = normal.y > 0.5;
    if (block_id == 3 && top) return 8;   // Grass top
    if (block_id == 5 && abs(normal.y) > 0.5) return 9;   // Wood end-grain
    if (block_id == 7 && top) return 10;  // Snow top
    return block_id;
}

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

    float diffuse = max(dot(N, L), 0.0);
    float shadow  = mix(1.0, sample_csm(N, L), u_shadow_strength);
    vec3 lighting = u_ambient_color + u_light_color * diffuse * shadow;

    int id = clamp(v_block_id, 0, 7);
    int tile = tile_for_face(id, N);
    vec3 albedo = sample_atlas(tile, v_uv);

    vec3 lit = albedo * lighting * v_ao;

    float d = length(v_world_pos - u_camera_pos);
    float f = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1e-4), 0.0, 1.0);
    vec3 final = mix(lit, u_fog_color, f);

    frag_color = vec4(final, 1.0);
}
