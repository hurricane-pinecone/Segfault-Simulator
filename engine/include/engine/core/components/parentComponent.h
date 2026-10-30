#pragma once

#include "engine/core/ecs/entity.h"

namespace sfs
{

/**
 * Makes the entity a child of another, so it moves with it: its
 * TransformComponent is relative to the parent's (see hierarchy.h). Set it
 * through hierarchy::setParent, which refuses loops; a root entity has none.
 *
 * Not reflected: scene files store the hierarchy as nesting, not as a field.
 *
 * @param Entity parent - the parent; a destroyed parent leaves this a root
 */
struct ParentComponent
{
  Entity parent;

  ParentComponent() = default;
  explicit ParentComponent(Entity parent) : parent(parent) {}
};

} // namespace sfs
