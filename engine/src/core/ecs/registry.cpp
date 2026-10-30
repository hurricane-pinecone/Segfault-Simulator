#include <engine/core/ecs/entity.h>
#include <engine/core/ecs/registry.h>
#include <engine/core/logger/logger.h>
#include <engine/core/util/fatalError.h>

#include <string>

namespace sfs
{

Entity Registry::createEntity()
{
  Entity::EntityId id;

  if (!freeEntityIds.empty())
  {
    id = *freeEntityIds.begin();
    freeEntityIds.erase(freeEntityIds.begin());
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

void Registry::addExistingEntities(System& system)
{
  const Signature& systemSignature = system.getComponentSignature();
  for (Entity::EntityId id = 0; id < entityCount; ++id)
  {
    if (!entityAlive[id])
      continue;

    const Entity entity = getEntity(id);
    if (entitiesToBeAdded.count(entity) > 0)
      continue;

    if ((entityComponentSignatures[id] & systemSignature) == systemSignature)
      system.addEntity(entity);
  }
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
  // Only the systems it could be in: those its components match.
  const Signature& entitySignature = entityComponentSignatures[entity.getId()];
  for (auto& system : systems)
  {
    const Signature& systemSignature = system->getComponentSignature();
    if ((entitySignature & systemSignature) == systemSignature)
      system->removeEntity(entity);
  }
}

void Registry::componentsChanged(const Entity& entity, const Signature& before)
{
  // Not in any system yet: it joins them, as it is then, at the next flush.
  if (entitiesToBeAdded.contains(entity))
    return;

  const Signature& after = entityComponentSignatures[entity.getId()];
  for (auto& system : systems)
  {
    const Signature& wanted = system->getComponentSignature();
    const bool was = (before & wanted) == wanted;
    const bool is = (after & wanted) == wanted;
    if (was && !is)
      system->removeEntity(entity);
    else if (is && !was)
      system->addEntity(entity);
  }
}

void Registry::badComponentAccess(const Entity& entity,
                                  const char* action,
                                  std::string_view type) const
{
  const std::string what = std::string(action) + "<" +
                           detail::readableTypeName(type) + ">: entity " +
                           std::to_string(entity.getId());
  const auto id = entity.getId();
  if (!entity.isValid())
    fatalError(what + " isn't an entity (a default-made Entity)");
  if (id >= entityAlive.size() || !entityAlive[id])
    fatalError(what + " was destroyed");
  if (generations[id] != entity.getGeneration())
    fatalError(what + " was destroyed (its id now belongs to another entity)");
  fatalError(what + " has no such component");
}

namespace detail
{
void invalidEntity(const char* action)
{
  fatalError(std::string(action) +
             " on an Entity that isn't one (default-made, or reset)");
}
} // namespace detail

void Registry::flushEntities()
{
  for (const auto& entity : entitiesToBeRemoved)
  {
    if (!isAlive(entity))
      continue;

    const auto id = entity.getId();

    // Still pending its first flush: in no system yet.
    if (!entitiesToBeAdded.contains(entity))
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

    freeEntityIds.insert(id);
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

std::vector<Entity> Registry::entities()
{
  std::vector<Entity> result;
  for (Entity::EntityId id = 0; id < entityCount; ++id)
    if (entityAlive[id])
      result.push_back(getEntity(id));
  return result;
}

bool Registry::isAlive(const Entity& entity) const
{
  const auto id = entity.getId();

  return id < entityAlive.size() && entityAlive[id] &&
         generations[id] == entity.getGeneration();
}

} // namespace sfs
