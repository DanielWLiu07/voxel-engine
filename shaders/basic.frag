#version 410 core

in vec3  v_normal_ws;
in vec2  v_uv;
in vec3  v_world_pos;
in float v_ao;
flat in int v_block_id;

uniform vec3  u_light_dir;
uniform vec3  u_light_color;
uniform vec3  u_ambient_color;
uniform vec3  u_camera_pos;
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

// Block palette indexed by BlockId. Keep in sync with world/block.h:
// 0=Air, 1=Stone, 2=Dirt, 3=Grass, 4=Sand, 5=Wood, 6=Leaves.
uniform vec3 u_palette[8];

out vec4 frag_color;

// Cheap deterministic hash from a 3D coord -> [0, 1). Used to give each
// block a small color jitter so identical-id surfaces don't read as a
// uniform sheet of paint.
float hash3(vec3 p) {
    p = fract(p * vec3(443.8975, 397.2973, 491.1871));
    p += dot(p, p.yxz + 19.19);
    return fract((p.x + p.y) * p.z);
}

void main() {
    vec3 N = normalize(v_normal_ws);
    vec3 L = normalize(u_light_dir);

    float diffuse = max(dot(N, L), 0.0);
    vec3 lighting = u_ambient_color + u_light_color * diffuse;

    // Block-typed color with a small per-block hash jitter.
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
