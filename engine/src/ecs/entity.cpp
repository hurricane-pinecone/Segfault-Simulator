#include "engine/ecs/entity.h"
#include "engine/ecs/registry.h"

namespace sfs
{

bool Entity::isAlive() const { return registry->isAlive(*this); }
} // namespace sfs
