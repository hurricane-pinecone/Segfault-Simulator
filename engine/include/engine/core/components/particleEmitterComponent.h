#pragma once

#include "glm/glm/ext/vector_float3.hpp"
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
 *                           ground), default 0
 * @param bool enabled - whether it emits, default true
 */
struct ParticleEmitterComponent
{
  std::string effect;
  glm::vec3 offset{0.0f};
  bool enabled = true;

  ParticleEmitterComponent() = default;

  ParticleEmitterComponent(std::string effect,
                           glm::vec3 offset = glm::vec3(0.0f),
                           bool enabled = true)
      : effect(std::move(effect)), offset(offset), enabled(enabled)
  {
  }
};

} // namespace sfs
