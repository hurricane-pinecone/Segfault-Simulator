#pragma once

#include "engine/core/ecs/registry.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/fatalError.h"
#include <algorithm>
#include <string>

namespace sfs
{

template <typename TComponent>
Pool<TComponent>* Registry::poolOf()
{
  const auto componentId =
      static_cast<std::size_t>(Component<TComponent>::getId());
  return componentId < componentPools.size()
             ? static_cast<Pool<TComponent>*>(componentPools[componentId].get())
             : nullptr;
}

template <typename TComponent, typename... TArgs>
void Registry::addComponent(const Entity& entity, TArgs&&... args)
{
  const auto componentId = Component<TComponent>::getId();
  if (!isAlive(entity))
    badComponentAccess(
        entity, "addComponent", detail::componentTypeKey<TComponent>());
  const auto entityId = entity.getId();

  if (static_cast<std::size_t>(componentId) >= componentPools.size())
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

  const Signature before = entityComponentSignatures[entityId];
  entityComponentSignatures[entityId].set(componentId, true);
  if (!before.test(componentId))
    componentsChanged(entity, before);
}

template <typename TComponent>
void Registry::removeComponent(const Entity& entity)
{
  const int componentId = Component<TComponent>::getId();
  if (!isAlive(entity))
    return;
  const auto entityId = entity.getId();
  const Signature before = entityComponentSignatures[entityId];
  if (!before.test(componentId))
    return;

  entityComponentSignatures[entityId].set(componentId, false);
  componentsChanged(entity, before);
  if (Pool<TComponent>* pool = poolOf<TComponent>())
    pool->remove(static_cast<std::size_t>(entityId));
}

template <typename... TComponents>
const std::vector<Entity>& Registry::query()
{
  Signature signature;
  (signature.set(Component<TComponents>::getId()), ...);
  return queryOf(signature);
}

template <typename TComponent>
TComponent* Registry::singleton()
{
  const std::size_t id =
      static_cast<std::size_t>(Component<TComponent>::getId());
  if (m_holderCount[id] == 0)
    return nullptr;
  if (m_holderCount[id] > 1 && !m_warnedNotSingle[id])
    warnNotSingle(id, detail::componentTypeKey<TComponent>());
  if (m_holder[id] == kNoHolder)
    m_holder[id] = findHolder(id);
  if (m_holder[id] == kNoHolder)
    return nullptr;
  Pool<TComponent>* pool = poolOf<TComponent>();
  return pool ? pool->find(m_holder[id]) : nullptr;
}

template <typename TComponent>
uint64_t Registry::componentChanges() const
{
  return m_componentChanges[static_cast<std::size_t>(
      Component<TComponent>::getId())];
}

template <typename TComponent>
bool Registry::hasComponent(const Entity& entity) const
{
  const int componentId = Component<TComponent>::getId();
  return isAlive(entity) &&
         entityComponentSignatures[entity.getId()].test(componentId);
}

template <typename TComponent>
TComponent& Registry::getComponent(const Entity& entity)
{
  Pool<TComponent>* pool = poolOf<TComponent>();
  TComponent* component =
      pool && isAlive(entity) ? pool->find(entity.getId()) : nullptr;
  if (!component)
    badComponentAccess(
        entity, "getComponent", detail::componentTypeKey<TComponent>());
  return *component;
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
  attach(*base);

  return *ptr;
}

template <typename TSystem>
void Registry::removeSystem()
{
  systems.erase(std::remove_if(systems.begin(),
                               systems.end(),
                               [](const std::unique_ptr<System>& system) {
                                 return dynamic_cast<TSystem*>(system.get()) !=
                                        nullptr;
                               }),
                systems.end());
}

template <typename TSystem>
bool Registry::hasSystem() const
{
  return std::any_of(systems.begin(),
                     systems.end(),
                     [](const std::unique_ptr<System>& system) {
                       return dynamic_cast<TSystem*>(system.get()) != nullptr;
                     });
}

template <typename TSystem>
TSystem& Registry::getSystem() const
{
  if (auto* casted = tryGetSystem<TSystem>())
  {
    return *casted;
  }

  // getSystem assumes the caller registered the system; reaching here is a
  // setup bug, not a recoverable runtime error: say which type is missing and
  // end the game (see fatalError()). Callers that may legitimately have no
  // such system use tryGetSystem.
  fatalError(std::string("getSystem: the scene has no ") +
             detail::readableTypeName(detail::componentTypeKey<TSystem>()) +
             " system (add it, or use tryGetSystem)");
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

template <typename Fn>
void Registry::forEachSystem(Fn&& fn)
{
  for (auto& system : systems)
    fn(*system);
}

} // namespace sfs
