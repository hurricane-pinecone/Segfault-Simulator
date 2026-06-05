#pragma once

#include "engine/runtime/rendering/modules/particles.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <string>

// Sample-game particle composition. The effect prefabs themselves (the blood
// layers, embers) are engine-provided -- see engine/core/particles/particlePrefabs.h
// (registerBloodEffects / emberEffect). This file holds only the game-specific
// way they're fired: a shotgun gore blast.

// Fire a full shotgun gore blast at a world tile. `direction` should be a unit
// vector (the shot's travel); `power` is the spray's impulse in tiles/sec. Each
// engine blood layer gets its own share of that impulse: the mist races slightly
// ahead, the spray takes the full push, and the heavy gobs get only a fraction
// so they lag behind and splat. `prefix` selects the registered colour set
// ("blood", "ichor", ...).
inline void spawnGore(sfs::ParticleEngine& particles,
                      glm::vec2 worldPos,
                      float elevation,
                      glm::vec2 direction,
                      float power,
                      const std::string& prefix = "blood")
{
  sfs::ParticleSpawnParams mist;
  mist.velocity = direction * (power * 1.15f);
  mist.velocityZ = 2.0f;

  sfs::ParticleSpawnParams spray;
  spray.velocity = direction * power;
  spray.velocityZ = 1.5f; // modest kick so droplets land within their lifetime

  sfs::ParticleSpawnParams gobs;
  gobs.velocity =
      direction * (power * 0.4f); // heavy chunks: much less velocity
  gobs.velocityZ = 2.5f;          // arc, but still lands within its lifetime

  sfs::ParticleSpawnParams drip;
  drip.velocity = direction * (power * 0.04f); // essentially drops in place
  drip.velocityZ = 1.5f;

  particles.spawnBurst(prefix + "_mist", worldPos, elevation, mist);
  particles.spawnBurst(prefix + "_spray", worldPos, elevation, spray);
  particles.spawnBurst(prefix + "_gobs", worldPos, elevation, gobs);
  particles.spawnBurst(prefix + "_drip", worldPos, elevation, drip);
}
