#pragma once

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

/**
 * A body moving on the ground plane: the velocity, in world units per second,
 * that MovementSystem integrates into the entity's TransformComponent each
 * frame, and that CollisionSystem stops against solid WorldColliders. The
 * engine's basic 2D body (RigidBody3DComponent is the physics engine's); a
 * game with richer motion builds on top of it.
 *
 * @param glm::vec2 velocity - initial velocity (world units/second), default 0
 */
struct RigidBody2DComponent
{
  glm::vec2 velocity{0.0f};

  // Where the body was on the ground plane before this frame's move (set by
  // MovementSystem), for resolving collisions by the side they came from.
  [[sfs::hide]] glm::vec2 previousPosition{0.0f};

  RigidBody2DComponent(glm::vec2 velocity = glm::vec2(0.0f))
      : velocity(velocity)
  {
  }
};

} // namespace sfs
