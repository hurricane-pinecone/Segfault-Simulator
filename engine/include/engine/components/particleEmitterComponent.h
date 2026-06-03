#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <string>
#include <utility>

namespace sfs
{

// Attach to an entity to continuously emit a registered particle effect that
// follows the entity. ParticleSystem reads the entity's TransformComponent (and
// ElevationComponent, if present) each frame to place the emitter.
struct ParticleEmitterComponent
{
  std::string effect;        // name of an effect registered on ParticleSystem
  glm::vec2 offset{0.0f, 0.0f}; // world-tile offset from the entity position
  float heightOffset = 0.0f; // additional emitter height above the ground
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
