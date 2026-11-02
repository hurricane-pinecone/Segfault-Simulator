#pragma once

#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * What a game object is told when its entity touches another in a 2D game
 * (see GameObject::onCollisionEnter2D).
 */
struct Collision2D
{
  Entity other; // the entity it touched
  // Which way the other pushed it, on the game's plane (e.g. (0, 1) standing
  // on a floor, if y runs up); zero for a trigger's overlap.
  glm::vec2 normal{0.0f};
};

/** A contact between two 2D colliders that began or ended during a step. */
struct Contact2D
{
  Entity a;
  Entity b;
  glm::vec2 normal{0.0f}; // pushing a away from b; zero for an overlap
  bool trigger = false;   // one of them is a trigger (nothing was blocked)
};

} // namespace sfs
