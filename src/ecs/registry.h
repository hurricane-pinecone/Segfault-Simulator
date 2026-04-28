#pragma once

#include "ecs/component.h"
#include "entity.h"
#include "logger/logger.h"
#include "pool.h"
#include "system.h"

#include <memory>
#include <set>
#include <string>
#include <typeindex>
#include <utility>

class Registry
{
public:
  Registry() = default;
  ~Registry() = default;

  Entity createEntity();
  void destroyEntity();

  void update(double deltaTime);

  template <typename TComponent, typename... TArgs>
  void addComponent(const Entity& entity, TArgs&&... args);

  template <typename TComponent>
  void removeComponent(const Entity& entity);

  template <typename TComponent>
  bool hasComponent(const Entity& entity) const;

  template <typename TComponent>
  TComponent& getComponent(const Entity& entity);

  template <typename TSystem, typename... TArgs>
  void addSystem(TArgs&&... args);

  template <typename TSystem>
  void removeSystem();

  template <typename TSystem>
  bool hasSystem() const;

  template <typename TSystem>
  TSystem& getSystem() const;

private:
  void addEntityToSystems(const Entity& entity);

private:
  int entityCount = 0;
  std::set<Entity> entitiesToBeAdded;
  std::set<Entity> entitiesToBeRemoved;

  // vector index = component type id
  // ComponentPool index = entity id
  std::vector<std::shared_ptr<IPool>> componentPools;

  // Vector or component signatures, which allows us to tell which components
  // are turned on per entity
  std::vector<Signature> entityComponentSignatures;

  std::unordered_map<std::type_index, std::shared_ptr<System>> systems;
};

template <typename TComponent, typename... TArgs>
void Registry::addComponent(const Entity& entity, TArgs&&... args)
{
  const auto componentId = Component<TComponent>::getId();
  const auto entityId = entity.getId();

  if (componentId >= componentPools.size())
  {
    componentPools.resize(componentId + 1, nullptr);
  }

  if (!componentPools[componentId])
  {
    componentPools[componentId] = std::make_shared<Pool<TComponent>>();
  }

  std::shared_ptr<Pool<TComponent>> componentPool =
      std::static_pointer_cast<Pool<TComponent>>(componentPools[componentId]);

  componentPool->set(entityId, std::forward<TArgs>(args)...);

  entityComponentSignatures[entityId].set(componentId, true);

  LOG_DEBUG("Component ID: " + std::to_string(componentId) +
            " added to entity ID: " + std::to_string(entityId));
}

template <typename TComponent>
void Registry::removeComponent(const Entity& entity)
{
  const int componentId = Component<TComponent>::getId();
  const int entityId = entity.getId();
  entityComponentSignatures[entityId].set(componentId, false);
}

template <typename TComponent>
bool Registry::hasComponent(const Entity& entity) const
{
  const int componentId = Component<TComponent>::getId();
  return entityComponentSignatures[entity.getId()].test(componentId);
}

template <typename TComponent>
TComponent& Registry::getComponent(const Entity& entity)
{
  const int componentId = Component<TComponent>::getId();
  auto pool =
      std::static_pointer_cast<Pool<TComponent>>(componentPools[componentId]);
  return pool->get(entity.getId());
}

template <typename TSystem, typename... TArgs>
void Registry::addSystem(TArgs&&... args)
{
  std::shared_ptr<TSystem> system =
      std::make_shared<TSystem>(std::forward<TArgs>(args)...);
  auto typeIndex = std::type_index(typeid(TSystem));
  systems.insert(std::make_pair(typeIndex, system));
}

template <typename TSystem>
void Registry::removeSystem()
{
  auto typeIndex = std::type_index(typeid(TSystem));
  systems.erase(typeIndex);
}

template <typename TSystem>
bool Registry::hasSystem() const
{
  auto typeIndex = std::type_index(typeid(TSystem));
  return systems.find(typeIndex) != systems.end();
}

template <typename TSystem>
TSystem& Registry::getSystem() const
{
  auto typeIndex = std::type_index(typeid(TSystem));
  auto system = systems.find(typeIndex);

  if (system == systems.end())
  {
    throw std::runtime_error("System does not exist");
  }

  return *std::static_pointer_cast<TSystem>(system->second);
}

/*
 * ENTITY TEMPLATE METHODS
 *
 * They need to go here to avoid curcular imports
 */
template <typename TComponent, typename... TArgs>
Entity& Entity::addComponent(TArgs&&... args)
{
  registry->addComponent<TComponent>(*this, std::forward<TArgs>(args)...);
  return *this;
}

template <typename TComponent>
void Entity::removeComponent() const
{
  registry->removeComponent<TComponent>(*this);
};

template <typename TComponent>
bool Entity::hasComponent() const
{
  return registry->hasComponent<TComponent>(*this);
};

template <typename TComponent>
TComponent& Entity::getComponent() const
{
  return registry->getComponent<TComponent>(*this);
}
