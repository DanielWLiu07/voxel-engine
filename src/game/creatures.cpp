#include "game/creatures.h"

#include "world/block.h"
#include "world/chunk.h"

#include <cmath>

namespace game {

namespace {

// The same cheap hash the atmosphere shaders use, so a creature's seed
// behaves like a mote's and nobody has to learn a second scheme.
float h1(float n) {
    const float s = std::sin(n * 12.9898f) * 43758.5453123f;
    return s - std::floor(s);
}

// Where the ground is under (x, z), or a sentinel when the column is not
// resident. Searched downward from the sky rather than up from the
// bedrock: the surface is near the top, and a creature standing on a
// hill should find the hilltop, not the cave floor under it.
constexpr int kNoGround = -1;

int ground_under(const world::World& wrld, int x, int z) {
    for (int y = world::kChunkSizeY - 1; y >= 1; --y) {
        if (world::is_solid(wrld.block_at(x, y, z))) return y;
    }
    return kNoGround;
}

}  // namespace

void Creatures::update(const world::World& wrld, const glm::vec3& camera,
                       float time_seconds, float radius) {
    live_ = 0;

    for (int i = 0; i < kCount; ++i) {
        const float id = static_cast<float>(i);
        const float r0 = h1(id), r1 = h1(id + 17.13f), r2 = h1(id + 41.71f);

        Creature c;
        c.kind = (r2 < 0.55f) ? 0 : 1;

        // A slow circuit, each on its own radius and clock, plus a
        // wobble so the path is not visibly a circle. Two creatures
        // never share one because the seed enters both terms.
        const float speed  = (c.kind == 0 ? 0.16f : 0.10f) + r0 * 0.09f;
        const float orbit  = radius * (0.25f + r1 * 0.70f);
        const float ang    = time_seconds * speed + r2 * 6.2831f;
        const float wobble = std::sin(time_seconds * (0.5f + r0) + r1 * 6.2831f);

        // Anchored to the camera so the same population follows the
        // player, wrapped the way the motes are: walk far enough and the
        // ones behind you are the ones ahead.
        const float wx = camera.x + std::cos(ang) * orbit + wobble * 2.0f;
        const float wz = camera.z + std::sin(ang) * orbit + wobble * 1.4f;

        const int bx = static_cast<int>(std::floor(wx));
        const int bz = static_cast<int>(std::floor(wz));
        const int gy = ground_under(wrld, bx, bz);
        // No resident column means the chunk has not streamed in yet.
        // Skipping is right rather than guessing a height: a creature
        // standing at y=0 in mid-air for the frame before its ground
        // arrives is worse than one that appears a frame late.
        if (gy == kNoGround) continue;

        c.pos = glm::vec3(wx, static_cast<float>(gy + 1), wz);
        // Facing follows the path: the tangent of the circuit it walks.
        c.heading = ang + 1.5707963f;
        // Gait. A hopper bounces; a strider ambles, so its body only
        // rocks. Driven by distance covered rather than by the clock, so
        // a slow creature does not jog on the spot.
        const float gait = (ang * orbit) * (c.kind == 0 ? 1.5f : 0.9f);
        c.bob = 0.5f + 0.5f * std::sin(gait);

        creatures_[static_cast<std::size_t>(live_)] = c;
        ++live_;
    }
}

}  // namespace game
