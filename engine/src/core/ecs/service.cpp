#include "engine/core/ecs/service.h"

#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/logger/logger.h"

namespace sfs
{

Entity Service::createEntity()
{
  if (m_registry) [[likely]]
    return m_registry->createEntity();

  LOG_ERROR("A service created an entity before it was added to a registry; "
            "nothing created");
  return {};
}

void Service::destroyEntity(const Entity& entity)
{
  if (m_registry)
    m_registry->destroyEntity(entity);
}

} // namespace sfs
