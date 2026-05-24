#pragma once

#include "component.h"
#include "entity.h"
#include "pool.h"
#include "system.h"
#include <algorithm>
#include <engine/logger/logger.h>

#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace sfs
{

class Registry
{
public:
  Entity createEntity();
  Entity getEntity(Entity::EntityId id);
  void destroyEntity(const Entity& entity);
  bool isAlive(const Entity& entity) const;

  void flushEntities();

  template <typename TComponent, typename... TArgs>
  void addComponent(const Entity& entity, TArgs&&... args);

  template <typename TComponent>
  void removeComponent(const Entity& entity);

  template <typename TComponent>
  bool hasComponent(const Entity& entity) const;

  template <typename TComponent>
  TComponent& getComponent(const Entity& entity);

  template <typename TSystem, typename... TArgs>
  TSystem& addSystem(TArgs&&... args);

  template <typename TSystem>
  void removeSystem();

  template <typename TSystem>
  bool hasSystem() const;

  template <typename TSystem>
  TSystem& getSystem() const;

  template <typename TSystem>
  TSystem* tryGetSystem() const;

  template <typename... TComponents>
  std::vector<Entity> view();

  template <typename Fn>
  void forEachSystem(Fn&& fn);

private:
  // TODO: Figure out how to hide registry.h from client completely. For now
  // this stops anything but Scene being able to create Registry.
  Registry() = default;
  ~Registry() = default;

  void addEntityToSystems(const Entity& entity);
  void removeEntityFromSystems(const Entity& entity);

  friend class Scene;

private:
  uint32_t entityCount = 0;
  uint64_t nextDebugId = 1;

  std::vector<uint32_t> freeEntityIds;
  std::vector<uint64_t> entityDebugIds;
  std::vector<uint32_t> generations;
  std::vector<bool> entityAlive;
  std::set<Entity> entitiesToBeAdded;
  std::set<Entity> entitiesToBeRemoved;

  // vector index = component type id
  // ComponentPool index = entity id
  std::vector<std::unique_ptr<IPool>> componentPools;

  // Vector of component signatures, which allows us to tell which components
  // are turned on per entity
  std::vector<Signature> entityComponentSignatures;

  std::vector<std::unique_ptr<System>> systems;
};

template <typename TComponent, typename... TArgs>
void Registry::addComponent(const Entity& entity, TArgs&&... args)
{
  const auto componentId = Component<TComponent>::getId();
  const auto entityId = entity.getId();

  if (componentId >= componentPools.size())
  {
    componentPools.resize(componentId + 1);
  }

  if (!componentPools[componentId])
  {
    componentPools[componentId] = std::make_unique<Pool<TComponent>>();
  }

  auto componentPool =
      static_cast<Pool<TComponent>*>(componentPools[componentId].get());

  componentPool->set(entityId, std::forward<TArgs>(args)...);

  entityComponentSignatures[entityId].set(componentId, true);
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
  auto* pool =
      static_cast<Pool<TComponent>*>(componentPools[componentId].get());
  return pool->get(entity.getId());
}

template <typename TSystem, typename... TArgs>
TSystem& Registry::addSystem(TArgs&&... args)
{
  auto system = std::make_unique<TSystem>(std::forward<TArgs>(args)...);
  system->setRegistry(this);

  TSystem* ptr = system.get();

  systems.emplace_back(std::move(system));

  System* base = ptr;
  base->create();

  return *ptr;
}

template <typename TSystem>
void Registry::removeSystem()
{
  systems.erase(
      std::remove_if(
          systems.begin(),
          systems.end(),
          [](const std::unique_ptr<System>& system)
          { return dynamic_cast<TSystem*>(system.get()) != nullptr; }),
      systems.end());
}

template <typename TSystem>
bool Registry::hasSystem() const
{
  return std::any_of(
      systems.begin(),
      systems.end(),
      [](const std::unique_ptr<System>& system)
      { return dynamic_cast<TSystem*>(system.get()) != nullptr; });
}

template <typename TSystem>
TSystem& Registry::getSystem() const
{
  for (const auto& system : systems)
  {
    if (auto* casted = dynamic_cast<TSystem*>(system.get()))
    {
      return *casted;
    }
  }

  throw std::runtime_error("System not found");
}

template <typename TSystem>
TSystem* Registry::tryGetSystem() const
{
  if (!this->hasSystem<TSystem>())
    return nullptr;

  for (const auto& system : systems)
  {
    if (auto* casted = dynamic_cast<TSystem*>(system.get()))
    {
      return casted;
    }
  }

  return nullptr;
}

template <typename... TComponents>
std::vector<Entity> Registry::view()
{
  Signature requiredSignature;

  (requiredSignature.set(Component<TComponents>::getId()), ...);

  std::vector<Entity> result;

  for (int entityId = 0; entityId < entityComponentSignatures.size();
       entityId++)
  {
    const Signature& entitySignature = entityComponentSignatures[entityId];

    if ((entitySignature & requiredSignature) == requiredSignature)
    {
      result.push_back(getEntity(entityId));
    }
  }

  return result;
}

template <typename Fn>
void Registry::forEachSystem(Fn&& fn)
{
  for (auto& system : systems)
    fn(*system);
}

/*
 * ENTITY TEMPLATE METHODS
 *
 * They need to go here to avoid curcular imports
 *
 * TODO: Potentially scrap these. I'm not sold on storing a registry pointer on
 * entity soley for syntactic suger
 */
template <typename TComponent, typename... TArgs>
Entity& Entity::addComponent(TArgs&&... args)
{
  registry->addComponent<TComponent>(*this, std::forward<TArgs>(args)...);
  return *this;
}

template <typename TComponent, typename... TArgs>
Entity& Entity::addTag(TArgs&&... args)
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

} // namespace sfs
