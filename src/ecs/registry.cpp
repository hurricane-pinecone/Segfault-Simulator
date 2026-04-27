#include "registry.h"
#include "entity.h"
#include "logger/logger.h"

#include <string>

Entity Registry::createEntity()
{
  int entityId;
  entityId = entityCount++;

  Entity entity(entityId);
  entity.setRegistry(this);
  entitiesToBeAdded.insert(entity);

  if (entityId >= entityComponentSignatures.size())
    entityComponentSignatures.resize(entityId + 1);

  LOG_DEBUG("Entity queued for creation. ID: " + std::to_string(entityId));

  return entity;
}

void Registry::addEntityToSystems(const Entity& entity)
{
  const int entityId = entity.getId();
  const auto& entityComponentSignature = entityComponentSignatures[entityId];

  for (auto& system : systems)
  {
    const auto& systemComponentSignature =
        system.second->getComponentSignature();

    bool isInterested = (entityComponentSignature & systemComponentSignature) ==
                        systemComponentSignature;
    if (isInterested)
      system.second->addEntity(entity);
  }
}

void Registry::update(double deltaTime)
{
  for (const auto e : entitiesToBeAdded)
  {
    addEntityToSystems(e);
  }
  if (entitiesToBeAdded.size() > 0)
  {
    LOG_DEBUG(std::to_string(entitiesToBeAdded.size()) +
              " entities added to Systems.");
  }
  entitiesToBeAdded.clear();

  for (auto& [type, system] : systems)
  {
    system->update(deltaTime);
  }
}
