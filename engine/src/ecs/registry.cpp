#include <engine/ecs/entity.h>
#include <engine/ecs/registry.h>
#include <engine/logger/logger.h>

namespace sfs
{

Entity Registry::createEntity()
{
  Entity::EntityId id;

  if (!freeEntityIds.empty())
  {
    id = freeEntityIds.back();
    freeEntityIds.pop_back();
  }
  else
  {
    id = entityCount++;

    entityComponentSignatures.resize(entityCount);
    generations.resize(entityCount, 0);
    entityDebugIds.resize(entityCount, 0);
    entityAlive.resize(entityCount, false);
  }

  entityAlive[id] = true;
  entityDebugIds[id] = nextDebugId++;

  Entity entity{id, generations[id], entityDebugIds[id]};
  entity.setRegistry(this);

  entitiesToBeAdded.insert(entity);

  return entity;
}

Entity Registry::getEntity(Entity::EntityId id)
{
  if (id >= entityCount)
    return {};

  if (!entityAlive[id])
    return {};

  Entity entity{id, generations[id], entityDebugIds[id]};
  entity.setRegistry(this);

  return entity;
}

void Registry::addEntityToSystems(const Entity& entity)
{
  const int entityId = entity.getId();
  const auto& entityComponentSignature = entityComponentSignatures[entityId];

  for (auto& system : systems)
  {
    const auto& systemComponentSignature = system->getComponentSignature();

    bool isInterested = (entityComponentSignature & systemComponentSignature) ==
                        systemComponentSignature;
    if (isInterested)
      system->addEntity(entity);
  }
}

void Registry::removeEntityFromSystems(const Entity& entity)
{
  for (auto& system : systems)
  {
    system->removeEntity(entity);
  }
}

void Registry::flushEntities()
{
  for (const auto& entity : entitiesToBeRemoved)
  {
    if (!isAlive(entity))
      continue;

    const auto id = entity.getId();

    removeEntityFromSystems(entity);

    if (id < entityComponentSignatures.size())
      entityComponentSignatures[id].reset();

    for (auto& pool : componentPools)
    {
      if (pool)
      {
        pool->remove(id);
      }
    }

    entityAlive[id] = false;
    generations[id]++;

    freeEntityIds.push_back(id);
  }

  entitiesToBeRemoved.clear();

  for (const auto& entity : entitiesToBeAdded)
  {
    if (isAlive(entity))
      addEntityToSystems(entity);
  }

  entitiesToBeAdded.clear();
}

void Registry::destroyEntity(const Entity& entity)
{
  if (!isAlive(entity))
    return;

  if (entitiesToBeRemoved.contains(entity))
    return;

  entitiesToBeRemoved.insert(entity);
}

bool Registry::isAlive(const Entity& entity) const
{
  const auto id = entity.getId();

  return id < entityAlive.size() && entityAlive[id] &&
         generations[id] == entity.getGeneration();
}

} // namespace sfs
