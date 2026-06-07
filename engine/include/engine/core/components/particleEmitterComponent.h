#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <string>
#include <utility>

namespace sfs
{

/**
 * Attach to an entity to continuously emit a registered particle effect that
 * follows it. The Particles module reads the entity's TransformComponent (and
 * ElevationComponent, if present) each frame to place the emitter.
 *
 * @param std::string effect - name of a registered particle effect
 * @param glm::vec2 offset - world-tile offset from the entity, default (0, 0)
 * @param float heightOffset - emitter height above the ground, default 0
 * @param bool enabled - whether it emits, default true
 */
struct ParticleEmitterComponent
{
  std::string effect;
  glm::vec2 offset{0.0f, 0.0f};
  float heightOffset = 0.0f;
  bool enabled = true;

  ParticleEmitterComponent() = default;

  ParticleEmitterComponent(std::string effect,
                           glm::vec2 offset = {0.0f, 0.0f},
                           float heightOffset = 0.0f,
                           bool enabled = true)
      : effect(std::move(effect)), offset(offset), heightOffset(heightOffset),
        enabled(enabled)
  {
  }
};

} // namespace sfs
