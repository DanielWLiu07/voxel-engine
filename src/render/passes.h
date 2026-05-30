#pragma once

#include "gfx/frustum.h"
#include "gfx/shader.h"
#include "gfx/shadow_map.h"
#include "gfx/cascaded_shadow_map.h"
#include "gfx/water.h"
#include "gfx/wireframe_cube.h"
#include "render/lighting.h"
#include "world/world.h"

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace render {

struct FrameView {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 light_vp[gfx::kNumCascades];
    float     cascade_far[gfx::kNumCascades];
    glm::vec3 camera_pos;
    int       window_w;
    int       window_h;
    float     fog_start;
    float     fog_end;
    float     time_seconds;
};

// cascade_update_mask: bit c set => redraw cascade c's depth this frame.
// Bits cleared keep the previous frame's depth-texture layer contents, which
// remain valid as long as the caller also reuses the matching light_vp[c].
void draw_shadow_pass(gfx::CascadedShadowMap& shadow_map,
                      const gfx::Shader& depth_shader,
                      const world::World& wrld,
                      const FrameView& fv,
                      const LightingFrame& light,
                      uint32_t cascade_update_mask = 0xFFFFFFFFu);

void draw_sky(const gfx::Shader& sky_shader, GLuint sky_vao,
              const FrameView& fv, const LightingFrame& light);

// Returns the visible chunks / triangles draw stats.
world::DrawStats draw_terrain(const gfx::Shader& terrain_shader,
                              gfx::CascadedShadowMap& shadow_map,
                              const world::World& wrld,
                              const FrameView& fv,
                              const LightingFrame& light,
                              const glm::vec3 palette[8],
                              const gfx::Frustum& view_frustum);

void draw_water(const gfx::Shader& water_shader, gfx::WaterPlane& water,
                const FrameView& fv, const LightingFrame& light,
                float sea_level);

// Wireframe outline on the targeted block (skipped if have_selection
// is false), then a screen-space crosshair.
void draw_crosshair_and_selection(const gfx::Shader& wireframe_shader,
                                  const gfx::WireframeCube& cube,
                                  const gfx::Shader& crosshair_shader,
                                  GLuint crosshair_vao,
                                  const FrameView& fv,
                                  bool have_selection,
                                  int selection_block_x,
                                  int selection_block_y,
                                  int selection_block_z);

}  // namespace render
