#pragma once

#include <glm/glm/ext/vector_float2.hpp>

namespace sfs
{

/**
 * A body moving on a 2D game's plane: PhysicsSystem2D speeds it up by its
 * gravity, moves it by its velocity each frame, and stops it against static
 * BoxCollider2Ds (with one of its own). Units are the game's (a flat game's
 * pixels, an isometric game's tiles) per second.
 *
 * @param glm::vec2 velocity - initial velocity, default 0
 * @param glm::vec2 gravity - how fast it speeds up falling, default 0 (a
 *                            top-down game); a platformer's points down
 */
struct RigidBody2DComponent
{
  glm::vec2 velocity{0.0f};
  glm::vec2 gravity{0.0f};

  // Where the body was on the plane before this frame's move (set by
  // PhysicsSystem2D).
  [[sfs::hide]] glm::vec2 previousPosition{0.0f};

  RigidBody2DComponent(glm::vec2 velocity = glm::vec2(0.0f),
                       glm::vec2 gravity = glm::vec2(0.0f))
      : velocity(velocity), gravity(gravity)
  {
  }
};

} // namespace sfs
