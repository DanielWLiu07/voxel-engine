#version 410 core

// Packed 12-byte chunk vertex (see gfx::VertexPacked): integer
// attributes, decoded here. Attribute 0 = x, z, normal index, ao level;
// attribute 1 = y, u, v.
layout(location = 0) in uvec4 a_xzna;
layout(location = 1) in uvec3 a_yuv;
// Attribute 2 is bound by the same VAO the colour pass uses; the depth
// pass needs the block id only to know what the wind moves.
layout(location = 2) in uvec2 a_block_light;

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
// DUPLICATED from basic.vert, deliberately: GLSL here has no #include,
// and a shadow cast from unswayed geometry detaches from the leaves
// casting it. If you change one, change both.
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
uniform mat4 u_light_vp;

void main() {
    vec3 a_position = vec3(float(a_xzna.x), float(a_yuv.x), float(a_xzna.y));
    vec4 world = u_model * vec4(a_position, 1.0);
    world.xyz += wind_offset(world.xyz, a_block_light.x);
    gl_Position = u_light_vp * world;
}
