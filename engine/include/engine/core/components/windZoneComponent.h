#pragma once

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

/**
 * A local wind, as Unity's spherical WindZone: within `radius` of its entity
 * (across the plane: its position's x and y), it adds `wind` to the scene's
 * Wind (scene data), fully at its middle and fading to nothing at its edge: a
 * fan, a draught through a gap. Particles carried by the wind
 * (ParticleEffectDesc::windInfluence) feel it where their emitter is. An
 * entity without a TransformComponent blows nowhere.
 *
 * @param glm::vec2 wind - which way and how fast it blows across the world's
 *        XY, in metres a second
 * @param float radius - how far from its entity it reaches, world units
 * @param bool enabled - whether it blows
 */
struct WindZoneComponent
{
  glm::vec2 wind{0.0f};
  [[sfs::range(0, 200)]] float radius = 10.0f;
  bool enabled = true;
};

} // namespace sfs
