#pragma once

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

/**
 * Linear velocity, in world units per second, that the movement system
 * integrates into the entity's TransformComponent each frame. The engine's
 * basic 2D body; a game with richer motion builds on top of it.
 *
 * @param glm::vec2 velocity - initial velocity (world units/second), default 0
 */
struct RigidBodyComponent
{
  glm::vec2 velocity;

  RigidBodyComponent(glm::vec2 velocity = glm::vec2(0.0, 0.0))
      : velocity(velocity)
  {
  }
};

} // namespace sfs
