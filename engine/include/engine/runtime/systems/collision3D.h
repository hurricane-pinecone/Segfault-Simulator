#pragma once

#include "engine/core/ecs/entity.h"

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
  bool trigger = false; // one of them is a trigger (nothing was blocked)
};

} // namespace sfs
