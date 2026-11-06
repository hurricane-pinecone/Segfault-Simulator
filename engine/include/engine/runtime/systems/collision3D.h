#pragma once

#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/** What a game object is told when its entity touches another (see GameObject).
 */
struct Collision3D
{
  Entity other; // the entity it touched
};

/** A contact between two physics bodies that began or ended during a step. */
struct Contact3D
{
  Entity a;
  Entity b;
  bool trigger = false;  // one of them is a trigger (nothing was blocked)
  glm::vec3 point{0.0f}; // where they first touched (began contacts)
};

} // namespace sfs
