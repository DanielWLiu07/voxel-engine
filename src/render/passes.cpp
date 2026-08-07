#include "render/passes.h"

#include "core/profiler.h"
#include "world/chunk.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace render {

void draw_shadow_pass(gfx::CascadedShadowMap& shadow_map,
                      const gfx::Shader& depth_shader,
                      const world::World& wrld,
                      const FrameView& fv,
                      const LightingFrame& light,
                      uint32_t cascade_update_mask) {
    ZoneScopedN("shadow_pass");
    if (light.shadow_strength <= 0.0f) return;
    if ((cascade_update_mask & ((1u << gfx::kNumCascades) - 1u)) == 0u) return;

    depth_shader.use();
    bool any_bound = false;
    for (int c = 0; c < gfx::kNumCascades; ++c) {
        if ((cascade_update_mask & (1u << c)) == 0u) continue;
        shadow_map.begin_pass(c);
        any_bound = true;
        depth_shader.set_mat4("u_light_vp", fv.light_vp[c]);
        gfx::Frustum light_frustum;
        light_frustum.from_view_proj(fv.light_vp[c]);
        wrld.draw_visible_with(light_frustum,
            [&](const glm::mat4& m) { depth_shader.set_mat4("u_model", m); });
    }
    if (any_bound) shadow_map.end_pass(fv.window_w, fv.window_h);
}

void draw_sky(const gfx::Shader& sky_shader, GLuint sky_vao,
              const FrameView& fv, const LightingFrame& light) {
    ZoneScopedN("sky_pass");
    // Strip translation so the sky never moves with the camera.
    glm::mat4 view_no_trans = fv.view;
    view_no_trans[3] = glm::vec4(0, 0, 0, 1);
    glm::mat4 inv_vp = glm::inverse(fv.proj * view_no_trans);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    sky_shader.use();
    sky_shader.set_mat4("u_inv_view_proj", inv_vp);
    sky_shader.set_vec3("u_sky_top", light.sky_top);
    sky_shader.set_vec3("u_sky_horizon", light.sky_horizon);
    sky_shader.set_vec3("u_sun_dir", light.sun_dir);
    sky_shader.set_vec3("u_sun_color", light.sun_color);
    glBindVertexArray(sky_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
}

world::DrawStats draw_terrain(const gfx::Shader& terrain_shader,
                              gfx::CascadedShadowMap& shadow_map,
                              const world::World& wrld,
                              const FrameView& fv,
                              const LightingFrame& light,
                              const glm::vec3 palette[world::kBlockPaletteSize],
                              const gfx::Frustum& view_frustum,
                              bool occlusion_cull) {
    ZoneScopedN("terrain_pass");
    terrain_shader.use();
    terrain_shader.set_mat4("u_view", fv.view);
    terrain_shader.set_mat4("u_proj", fv.proj);
    terrain_shader.set_vec3("u_light_dir", light.light_dir);
    terrain_shader.set_vec3("u_light_color", light.sun_color);
    terrain_shader.set_vec3("u_ambient_color", light.ambient);
    terrain_shader.set_vec3("u_camera_pos", fv.camera_pos);
    terrain_shader.set_vec3("u_fog_color", light.sky_horizon);
    terrain_shader.set_float("u_fog_start", fv.fog_start);
    terrain_shader.set_float("u_fog_end", fv.fog_end);
    terrain_shader.set_int("u_shadow_array", 1);
    terrain_shader.set_float("u_shadow_strength", light.shadow_strength);
    shadow_map.bind_depth_array(1);
    // u_atlas already bound to unit 0 by the caller; remind the shader.
    terrain_shader.set_int("u_atlas", 0);

    terrain_shader.set_mat4_array("u_light_vp", fv.light_vp, gfx::kNumCascades);
    terrain_shader.set_float_array("u_cascade_far", fv.cascade_far,
                                   gfx::kNumCascades);
    terrain_shader.set_vec3_array("u_palette", palette,
                                  world::kBlockPaletteSize);

    if (occlusion_cull) {
        return wrld.draw_visible_occluded(view_frustum, fv.camera_pos,
                                          terrain_shader);
    }
    return wrld.draw_visible(view_frustum, terrain_shader);
}

void draw_water(const gfx::Shader& water_shader, gfx::WaterPlane& water,
                const FrameView& fv, const LightingFrame& light,
                float sea_level) {
    ZoneScopedN("water_pass");
    // Snap to player's chunk-aligned XZ so the finite plane covers the
    // streaming world; waves still read post-translation XZ.
    glm::vec3 origin(
        std::floor(fv.camera_pos.x / world::kChunkSizeX) * world::kChunkSizeX,
        0.0f,
        std::floor(fv.camera_pos.z / world::kChunkSizeZ) * world::kChunkSizeZ);
    glm::mat4 model = glm::translate(glm::mat4(1.0f), origin);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    water_shader.use();
    water_shader.set_mat4("u_model", model);
    water_shader.set_mat4("u_view", fv.view);
    water_shader.set_mat4("u_proj", fv.proj);
    water_shader.set_float("u_time", fv.time_seconds);
    water_shader.set_float("u_sea_level", sea_level);
    water_shader.set_vec3("u_camera_pos", fv.camera_pos);
    // Authored for the HDR/ACES pipeline (the original values predated it
    // and tonemapped to a washed gray film): a saturated deep blue body
    // and a sky-cyan glancing tone, both below 1.0 so bloom never grabs
    // the water itself - only the sun glints. Scaled by the day cycle
    // against noon daylight so night water darkens with the world
    // instead of glowing noon blue.
    const glm::vec3 daylight =
        light.ambient + light.sun_color * std::max(light.sun_height, 0.0f);
    const float day_scale = glm::clamp(
        (daylight.r + daylight.g + daylight.b) / (3.0f * kNoonDaylightMean),
        0.03f, 1.0f);
    water_shader.set_vec3("u_deep_color",
                          day_scale * glm::vec3(0.016f, 0.10f, 0.22f));
    water_shader.set_vec3("u_shallow_color",
                          day_scale * glm::vec3(0.15f, 0.42f, 0.60f));
    water_shader.set_vec3("u_sun_dir", light.sun_dir);
    water_shader.set_vec3("u_sun_color", light.sun_color);
    water_shader.set_vec3("u_fog_color", light.sky_horizon);
    water_shader.set_float("u_fog_start", fv.fog_start);
    water_shader.set_float("u_fog_end", fv.fog_end);
    water_shader.set_float("u_alpha", 0.95f);
    water.draw();

    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void draw_crosshair_and_selection(const gfx::Shader& wireframe_shader,
                                  const gfx::WireframeCube& cube,
                                  const gfx::Shader& crosshair_shader,
                                  GLuint crosshair_vao,
                                  const FrameView& fv,
                                  bool have_selection,
                                  int selection_block_x,
                                  int selection_block_y,
                                  int selection_block_z) {
    if (have_selection) {
        // Inflate the unit cube slightly so the outline sits above the
        // block face and avoids z-fighting.
        const float bias  = 0.005f;
        const float scale = 1.0f + 2.0f * bias;
        glm::vec3 origin(
            static_cast<float>(selection_block_x) - bias,
            static_cast<float>(selection_block_y) - bias,
            static_cast<float>(selection_block_z) - bias);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), origin)
                        * glm::scale(glm::mat4(1.0f), glm::vec3(scale));

        GLfloat prev_line_width = 1.0f;
        glGetFloatv(GL_LINE_WIDTH, &prev_line_width);
        glLineWidth(2.0f);

        wireframe_shader.use();
        wireframe_shader.set_mat4("u_model", model);
        wireframe_shader.set_mat4("u_view",  fv.view);
        wireframe_shader.set_mat4("u_proj",  fv.proj);
        wireframe_shader.set_vec3("u_color", glm::vec3(0.0f));
        cube.draw();

        glLineWidth(prev_line_width);
    }

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    crosshair_shader.use();
    crosshair_shader.set_vec2("u_screen_size",
                              {static_cast<float>(fv.window_w),
                               static_cast<float>(fv.window_h)});
    crosshair_shader.set_float("u_arm_px",    12.0f);
    crosshair_shader.set_float("u_stroke_px", 1.0f);

    glBindVertexArray(crosshair_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

}  // namespace render
