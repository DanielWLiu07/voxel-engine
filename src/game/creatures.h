#pragma once

#include "world/world.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace game {

// Wandering creatures, on the same procedural plan the atmosphere uses.
//
// Position is a pure function of a seed and the clock - there is no
// simulation state, nothing to save, and nothing to get out of step with
// a reloaded world or a rotated 4D slice. That is not only tidiness: the
// engine's frame `dt` is real wall-clock time even during a capture, so
// anything integrated frame by frame would give a different answer every
// run, and every committed still here is checked against the command in
// its caption.
//
// What they do NOT do, for the same reason, is react to the player.
// Making one flee would mean carrying state, which would put it back in
// the path of that check. The wandering is the visual half of being
// alive; reacting is a separate job with a real cost attached.
//
// The one thing sampled rather than computed is the ground. A creature
// reads the terrain height under itself each frame and stands on it, so
// it walks over hills instead of through them - and when the 4D cut
// turns and the terrain beneath it becomes a different landscape, it
// steps onto the new one.
struct Creature {
    glm::vec3 pos{};        // feet, world space
    float     heading = 0;  // radians, the way it faces
    float     bob = 0;      // gait, 0..1
    int       kind = 0;     // 0 hopper, 1 strider
};

class Creatures {
public:
    // How many wander the world at once. They are placed around the
    // player and wrap with them, so this is a density rather than a
    // population: the same count follows you everywhere.
    static constexpr int kCount = 16;

    // Recomputes every creature for this instant. `radius` is how far
    // from the camera they roam.
    void update(const world::World& wrld, const glm::vec3& camera,
                float time_seconds, float radius = 34.0f);

    const std::array<Creature, kCount>& all() const { return creatures_; }
    int live_count() const { return live_; }

private:
    std::array<Creature, kCount> creatures_{};
    int live_ = 0;
};

}  // namespace game
