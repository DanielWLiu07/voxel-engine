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
    // The same wind the colour pass applies, or the canopy's shadow stays
    // where the canopy no longer is.
    depth_shader.set_float("u_time", fv.time_seconds);
    depth_shader.set_float("u_wind", fv.wind);
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

void draw_motes(const gfx::Shader& motes_shader, GLuint vao,
                const FrameView& fv, const LightingFrame& light) {
    ZoneScopedN("motes_pass");
    if (fv.motes <= 0.0f) return;

    // How many, and how far they reach. 3000 in a 48 m half-box is dense
    // enough to read as a field at night and invisible enough by day; the
    // whole pass is one draw call either way.
    constexpr int  kCount = 9000;
    // Wide and thin: the band of air a player is actually standing in.
    // A cube of the same reach puts most of them overhead, where they
    // read as stray pixels against the sky instead of as fireflies.
    const glm::vec3 kBoxHalfExtent(30.0f, 6.0f, 30.0f);

    motes_shader.use();
    motes_shader.set_mat4("u_view", fv.view);
    motes_shader.set_mat4("u_proj", fv.proj);
    motes_shader.set_vec3("u_camera_pos", fv.camera_pos);
    motes_shader.set_float("u_time", fv.time_seconds);
    motes_shader.set_float("u_night", light.star_fade);
    motes_shader.set_vec3("u_box", kBoxHalfExtent);
    motes_shader.set_float("u_viewport_h", static_cast<float>(fv.window_h));
    motes_shader.set_float("u_strength", fv.motes);

    // Depth TEST on, depth WRITE off: terrain in front of a mote hides it,
    // but a mote never occludes anything behind it - two overlapping ones
    // should both show.
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);      // additive
    glDepthMask(GL_FALSE);

    glBindVertexArray(vao);
    glDrawArrays(GL_POINTS, 0, kCount);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glDisable(GL_PROGRAM_POINT_SIZE);
}

void draw_precip(const gfx::Shader& precip_shader, GLuint vao,
                 const FrameView& fv) {
    ZoneScopedN("precip_pass");
    if (fv.precip <= 0.001f) return;

    // Snow is sparser and slower, so fewer of them read as more. Rain
    // costs two vertices a drop because each one is a line segment.
    const int kDrops = fv.precip_snow ? 4500 : 7000;
    const glm::vec3 kBoxHalfExtent = fv.precip_snow
                                         ? glm::vec3(26.0f, 20.0f, 26.0f)
                                         : glm::vec3(22.0f, 22.0f, 22.0f);

    precip_shader.use();
    precip_shader.set_mat4("u_view", fv.view);
    precip_shader.set_mat4("u_proj", fv.proj);
    precip_shader.set_vec3("u_camera_pos", fv.camera_pos);
    precip_shader.set_float("u_time", fv.time_seconds);
    precip_shader.set_vec3("u_box", kBoxHalfExtent);
    precip_shader.set_float("u_viewport_h", static_cast<float>(fv.window_h));
    precip_shader.set_float("u_strength", fv.precip);
    precip_shader.set_int("u_snow", fv.precip_snow ? 1 : 0);

    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glBindVertexArray(vao);
    if (fv.precip_snow) {
        glDrawArrays(GL_POINTS, 0, kDrops);
    } else {
        glDrawArrays(GL_LINES, 0, kDrops * 2);
    }
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_PROGRAM_POINT_SIZE);
}

void draw_birds(const gfx::Shader& birds_shader, GLuint vao,
                const FrameView& fv, const LightingFrame& light) {
    ZoneScopedN("birds_pass");
    const float day = 1.0f - light.star_fade;
    if (fv.birds <= 0.0f || day <= 0.01f) return;

    // 60 birds in flocks of 12. Enough that the sky is not empty, few
    // enough that they stay birds rather than a swarm.
    constexpr int kBirds = 60;

    birds_shader.use();
    birds_shader.set_mat4("u_view", fv.view);
    birds_shader.set_mat4("u_proj", fv.proj);
    birds_shader.set_vec3("u_camera_pos", fv.camera_pos);
    birds_shader.set_float("u_time", fv.time_seconds);
    birds_shader.set_float("u_day", day);
    birds_shader.set_float("u_strength", fv.birds);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, kBirds * 4);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void draw_sky(const gfx::Shader& sky_shader, GLuint sky_vao,
              const FrameView& fv, const LightingFrame& light,
              bool depth_test) {
    ZoneScopedN("sky_pass");
    // Strip translation so the sky never moves with the camera.
    glm::mat4 view_no_trans = fv.view;
    view_no_trans[3] = glm::vec4(0, 0, 0, 1);
    glm::mat4 inv_vp = glm::inverse(fv.proj * view_no_trans);

    // The sky runs AFTER the terrain, not before it. Its triangle sits on
    // the far plane, so with the depth test left on and flipped to LEQUAL
    // it survives only where nothing was drawn - the shader never runs on
    // a pixel the world already covers. That inversion is what pays for
    // the clouds and stars: the pass got more expensive per pixel and
    // cheaper per frame, because a typical view is mostly ground.
    // Depth writes stay off; the sky must not occlude the water pass.
    glDepthMask(GL_FALSE);
    if (depth_test) glDepthFunc(GL_LEQUAL);
    else            glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    sky_shader.use();
    sky_shader.set_mat4("u_inv_view_proj", inv_vp);
    sky_shader.set_vec3("u_sky_top", light.sky_top);
    sky_shader.set_vec3("u_sky_horizon", light.sky_horizon);
    sky_shader.set_vec3("u_sun_dir", light.sun_dir);
    sky_shader.set_vec3("u_sun_color", light.sun_color);
    sky_shader.set_vec3("u_moon_dir", light.moon_dir);
    sky_shader.set_float("u_star_fade", light.star_fade);
    sky_shader.set_float("u_time", fv.time_seconds);
    sky_shader.set_float("u_aurora", fv.aurora);
    sky_shader.set_mat3("u_star_rot", light.star_rot);
    glBindVertexArray(sky_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (depth_test) glDepthFunc(GL_LESS);
    else            glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
}

world::DrawStats draw_terrain_wireframe(const gfx::Shader& wire_shader,
                                        const world::World& wrld,
                                        const FrameView& fv,
                                        const gfx::Frustum& frustum,
                                        const glm::vec3& color) {
    ZoneScopedN("terrain_wireframe_pass");
    wire_shader.use();
    wire_shader.set_mat4("u_view", fv.view);
    wire_shader.set_mat4("u_proj", fv.proj);
    wire_shader.set_vec3("u_color", color);
    return wrld.draw_visible_with(frustum, [&](const glm::mat4& m) {
        wire_shader.set_mat4("u_model", m);
    });
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
    // Whole run lengths for the cube mesher, fractions of a block for the
    // prism mesher. An unset uniform is zero, which would collapse every
    // texture coordinate to a single texel, so this is not optional.
    terrain_shader.set_float("u_uv_scale", wrld.mesh_uv_scale());
    terrain_shader.set_float("u_time", fv.time_seconds);
    terrain_shader.set_float("u_wind", fv.wind);
    terrain_shader.set_float("u_mist", fv.mist);
    terrain_shader.set_float("u_mist_level", fv.mist_level);
    // The mist takes its colour from the horizon, so it agrees with the
    // sky it fades into and greys over with the weather along with it.
    terrain_shader.set_vec3("u_mist_color", light.sky_horizon);

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
