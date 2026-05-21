#pragma once

#include "gfx/frustum.h"
#include "gfx/shader.h"
#include "gfx/shadow_map.h"
#include "gfx/water.h"
#include "render/lighting.h"
#include "world/world.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

namespace render {

struct FrameView {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 light_vp;
    glm::vec3 camera_pos;
    int       window_w;
    int       window_h;
    float     fog_start;
    float     fog_end;
    float     time_seconds;
};

void draw_shadow_pass(gfx::ShadowMap& shadow_map,
                      const gfx::Shader& depth_shader,
                      const world::World& wrld,
                      const FrameView& fv,
                      const LightingFrame& light);

void draw_sky(const gfx::Shader& sky_shader, GLuint sky_vao,
              const FrameView& fv, const LightingFrame& light);

// Returns the visible chunks / triangles draw stats.
world::DrawStats draw_terrain(const gfx::Shader& terrain_shader,
                              gfx::ShadowMap& shadow_map,
                              const world::World& wrld,
                              const FrameView& fv,
                              const LightingFrame& light,
                              const glm::vec3 palette[8],
                              const gfx::Frustum& view_frustum);

void draw_water(const gfx::Shader& water_shader, gfx::WaterPlane& water,
                const FrameView& fv, const LightingFrame& light,
                float sea_level);

}  // namespace render
