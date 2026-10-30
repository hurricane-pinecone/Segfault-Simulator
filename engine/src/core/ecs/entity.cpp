#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"

namespace sfs
{

bool Entity::isAlive() const
{
  return registry != nullptr && registry->isAlive(*this);
}
} // namespace sfs
