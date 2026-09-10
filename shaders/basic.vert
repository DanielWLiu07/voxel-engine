#version 410 core

// Packed 12-byte chunk vertex (see gfx::VertexPacked): integer
// attributes, decoded here. Attribute 0 = x, z, normal index, ao level;
// attribute 1 = y, u, v; attribute 2 = block id, block light 0..15.
layout(location = 0) in uvec4 a_xzna;
layout(location = 1) in uvec3 a_yuv;
layout(location = 2) in uvec2 a_block_light;

const vec3 kNormals[6] = vec3[6](
    vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),
    vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));

// Indices 6..255 are 250 horizontal directions, for the walls of a 4D
// block's cross-section - see gfx::decode_packed_normal, which this
// mirrors exactly. A voxel face needs six normals; a hexagonal pillar's
// sides face wherever the cut left them, and they are always horizontal
// because y is the axis the slice rotation leaves alone.
vec3 decode_normal(uint idx) {
    if (idx < 6u) return kNormals[idx];
    float a = float(idx - 6u) * (6.28318530717958648 / 250.0);
    return vec3(cos(a), 0.0, sin(a));
}

// AO brightness per 0..3 occlusion level (was baked CPU-side pre-packing).
const float kAoBrightness[4] = float[4](0.45, 0.65, 0.82, 1.00);

// Wind, applied in world space.
//
// Leaves only. A swaying ground block would tear the terrain open, and
// a trunk that moved with its canopy would slide out of the ground.
//
// The offset is a pure function of world position and time, which is the
// property that keeps it seamless: two vertices at the same world point
// get the same offset, so no gap opens along a greedy-merged quad's edge
// or where a canopy meets the trunk it hangs on. Amplitude stays small
// for the same reason - merging turns a canopy into large quads whose
// corners then move independently, and a big amplitude would shear them
// visibly rather than sway them.
//
// DUPLICATED in shadow_depth.vert, deliberately: GLSL here has no
// #include, and a shadow cast from unswayed geometry detaches from the
// leaves casting it. If you change one, change both.
uniform float u_time;
uniform float u_wind;      // 0 disables; captures pin u_time instead

vec3 wind_offset(vec3 world_pos, uint block_id) {
    if (block_id != 6u) return vec3(0.0);   // world::BlockId::Leaves
    float t = u_time * 1.1;
    float phase = world_pos.x * 0.13 + world_pos.z * 0.09;
    // Two frequencies so a canopy does not read as one sine wave.
    float s = sin(t + phase) * 0.7 + sin(t * 1.9 + phase * 2.3) * 0.3;
    return vec3(s * 0.09, 0.0, s * 0.06) * u_wind;
}

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform mat4 u_light_vp[3];
// 1.0 for the cube mesher, whose uv is a whole run length; 1/64 for
// prism meshes, whose uv runs across a fraction of a block.
uniform float u_uv_scale;

out vec3  v_normal_ws;
out vec2  v_uv;
out vec3  v_world_pos;
out vec3  v_view_pos;
out vec4  v_light_pos[3];
out float v_ao;
out float v_light;
flat out int v_block_id;

void main() {
    vec3 a_position = vec3(float(a_xzna.x), float(a_yuv.x), float(a_xzna.y));
    vec3 a_normal   = decode_normal(a_xzna.z);
    vec2 a_uv       = vec2(float(a_yuv.y), float(a_yuv.z)) * u_uv_scale;
    float a_ao      = kAoBrightness[a_xzna.w];
    vec4 world = u_model * vec4(a_position, 1.0);
    world.xyz += wind_offset(world.xyz, a_block_light.x);
    v_world_pos = world.xyz;
    v_view_pos  = (u_view * world).xyz;
    v_normal_ws = mat3(u_model) * a_normal;
    v_uv = a_uv;
    v_ao = a_ao;
    v_block_id = int(a_block_light.x);
    // Block light 0..15 -> a brightness floor. Even an unlit face keeps
    // some ambient, or caves would be pure black rather than dark.
    v_light = float(a_block_light.y) / 15.0;
    for (int i = 0; i < 3; ++i) {
        v_light_pos[i] = u_light_vp[i] * world;
    }
    gl_Position = u_proj * u_view * world;
}
