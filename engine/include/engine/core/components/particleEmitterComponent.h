#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float3.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <string>
#include <utility>

namespace sfs
{

/**
 * Attach to an entity to continuously emit a registered particle effect that
 * follows it, from the entity's TransformComponent (its z: the height it's
 * at) plus the offset.
 *
 * @param std::string effect - name of a registered particle effect
 * @param glm::vec3 offset - world offset from the entity (z: height above the
 *                           ground), default 0; turned with the entity when
 *                           turnWithEntity is on
 * @param bool enabled - whether it emits, default true
 * @param float rate - how much of the effect's emitRate it emits (1 as
 *                     authored, 0 none, 2 twice as many): a stream fed by
 *                     something that varies, set from a script
 * @param bool turnWithEntity - the offset, the effect's emission shape and
 *                              its launch direction turn with the entity's
 *                              heading (its turn about the up axis): the
 *                              effect's +X is the entity's +x. Off, they stay
 *                              in the world's axes
 * @param float inheritVelocity - how much of the entity's own velocity its
 *                                particles leave with (0 none, the default; 1
 *                                all: what's poured from a moving machine
 *                                moves on with it)
 * @param glm::vec4 tint - multiplies the effect's colour (rgb) and alpha,
 *                         default white: one effect in many colours
 * @param glm::vec2 shapeScale - scales where the effect's emission shape
 *                               spawns particles, per axis (before it turns
 *                               with the entity), default 1: one effect over
 *                               any width, e.g. a box as wide as a machine
 * @param int maxParticles - how many particles this emitter may have alive
 *                           at once (0, the default: the effect's own
 *                           maxParticles)
 */
struct ParticleEmitterComponent
{
  std::string effect;
  glm::vec3 offset{0.0f};
  bool enabled = true;
  [[sfs::range(0, 4)]] float rate = 1.0f;
  bool turnWithEntity = false;
  [[sfs::range(0, 1)]] float inheritVelocity = 0.0f;
  [[sfs::color]] glm::vec4 tint{1.0f};
  glm::vec2 shapeScale{1.0f};
  int maxParticles = 0;

  ParticleEmitterComponent() = default;

  ParticleEmitterComponent(std::string effect,
                           glm::vec3 offset = glm::vec3(0.0f),
                           bool enabled = true)
      : effect(std::move(effect)), offset(offset), enabled(enabled)
  {
  }
};

} // namespace sfs
