#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * A wind that carries particles, as Unity's directional WindZone does. It
 * blows the same everywhere, so where its entity is doesn't matter. Each
 * update the particles add up every enabled zone's wind, and an effect with a
 * windInfluence (ParticleEffectDesc) is carried toward that share of it by
 * its drag. A game's weather writes it.
 *
 * @param glm::vec2 wind - which way and how fast it blows across the world's
 *        XY, in metres a second
 * @param bool enabled - whether it blows
 */
struct WindZoneComponent
{
  glm::vec2 wind{0.0f};
  bool enabled = true;
};

} // namespace sfs
