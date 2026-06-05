#pragma once

#include "engine/core/ecs/registry.h"
#include <algorithm>

namespace sfs
{

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

} // namespace sfs
