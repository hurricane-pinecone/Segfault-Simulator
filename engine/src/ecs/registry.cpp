#include <engine/ecs/entity.h>
#include <engine/ecs/registry.h>
#include <engine/logger/logger.h>

#include <string>

namespace sfs
{

Entity Registry::createEntity()
{
  int entityId;
  entityId = entityCount++;

  Entity entity(entityId);
  entity.setRegistry(this);

  entitiesToBeAdded.insert(entity);

  if (entityId >= entityComponentSignatures.size())
    entityComponentSignatures.resize(entityId + 1);

  return entity;
}

Entity Registry::getEntity(int id)
{
  if (id < 0 || id >= entityCount)
    return Entity();

  Entity entity(id);
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

void Registry::update(double deltaTime)
{
  for (const auto e : entitiesToBeAdded)
  {
    addEntityToSystems(e);
  }
  entitiesToBeAdded.clear();

  for (const auto e : entitiesToBeRemoved)
  {
    Entity entity(e.id);
    entity.setRegistry(this);

    removeEntityFromSystems(entity);

    if (e.id >= 0 && e.id < entityComponentSignatures.size())
      entityComponentSignatures[e.id].reset();
  }

  entitiesToBeRemoved.clear();

  for (auto& system : systems)
  {
    system->update(deltaTime);
  }
}

void Registry::destroyEntity(int id)
{
  if (id < 0 || id >= entityCount)
    return;

  entitiesToBeRemoved.insert(id);
}

} // namespace sfs
