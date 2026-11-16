#pragma once

#include "engine/core/ecs/registry.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/core/util/fatalError.h"
#include <algorithm>
#include <string>
#include <typeinfo>

namespace sfs
{

template <typename TComponent>
Pool<std::remove_cv_t<TComponent>>* Registry::poolOf()
{
  using Stored = std::remove_cv_t<TComponent>;
  const auto componentId = static_cast<std::size_t>(Component<Stored>::getId());
  return componentId < componentPools.size()
             ? static_cast<Pool<Stored>*>(componentPools[componentId].get())
             : nullptr;
}

template <typename TComponent, typename... TArgs>
void Registry::add(const char* action, const Entity& entity, TArgs&&... args)
{
  static_assert(!std::is_const_v<TComponent> && !std::is_volatile_v<TComponent>,
                "add the component itself, not a const one: const is for "
                "reading it (getComponent<const T>, a const query term)");
  const auto componentId = Component<TComponent>::getId();
  if (!isValid(entity))
  {
    refuseAdd(entity, action, detail::componentTypeKey<TComponent>());
    return;
  }
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
    componentsChanged(entity, before, static_cast<std::size_t>(componentId));
}

template <typename TComponent>
void Registry::removeComponent(const Entity& entity)
{
  static_assert(!std::is_const_v<TComponent> && !std::is_volatile_v<TComponent>,
                "remove the component itself, not a const one: const is for "
                "reading it (getComponent<const T>, a const query term)");
  const int componentId = Component<TComponent>::getId();
  if (!isValid(entity))
    return;
  const auto entityId = entity.getId();
  const Signature before = entityComponentSignatures[entityId];
  if (!before.test(componentId))
    return;

  entityComponentSignatures[entityId].set(componentId, false);
  componentsChanged(entity, before, static_cast<std::size_t>(componentId));
  if (Pool<TComponent>* pool = poolOf<TComponent>())
    pool->remove(static_cast<std::size_t>(entityId));
}

template <typename... TTerms>
QueryView<TTerms...> Registry::query()
{
  return QueryView<TTerms...>(
      this, &m_queryLists[listOf(detail::querySignature<TTerms...>())]);
}

template <typename T>
T* Registry::sceneData()
{
  (void)sizeof(detail::RequireSceneData<T>);
  const std::size_t id = detail::sceneDataId<T>();
  return id < m_sceneData.size()
             ? static_cast<std::remove_cv_t<T>*>(m_sceneData[id].get())
             : nullptr;
}

template <typename T, typename... TArgs>
T& Registry::addSceneData(TArgs&&... args)
{
  (void)sizeof(detail::RequireSceneData<T>);
  static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
                "add the scene data itself, not a const one: const is for "
                "reading it (sceneData<const T>())");
  const std::size_t id = detail::sceneDataId<T>();
  while (m_sceneData.size() <= id)
    m_sceneData.emplace_back(nullptr, [](void*) {});
  if (m_sceneData[id])
  {
    sceneDataExists(detail::componentTypeKey<T>());
    return *static_cast<T*>(m_sceneData[id].get());
  }
  T* made = nullptr;
  if constexpr (std::is_constructible_v<T, TArgs&&...>)
    made = new T(std::forward<TArgs>(args)...);
  else
    made = new T{std::forward<TArgs>(args)...};
  m_sceneData[id] = {made, [](void* data) { delete static_cast<T*>(data); }};
  return *made;
}

template <typename T>
void Registry::removeSceneData()
{
  (void)sizeof(detail::RequireSceneData<T>);
  static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
                "remove the scene data itself, not a const one");
  if (const std::size_t id = detail::sceneDataId<T>(); id < m_sceneData.size())
    m_sceneData[id].reset();
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
  return isValid(entity) &&
         entityComponentSignatures[entity.getId()].test(componentId);
}

template <typename TSystem, typename... TArgs>
TSystem& Registry::addSystem(TArgs&&... args)
{
  auto system = std::make_unique<TSystem>(std::forward<TArgs>(args)...);
  system->setRegistry(this);
  detail::EventHost::attach(*system, &m_events);

  TSystem* ptr = system.get();

  systems.emplace_back(std::move(system));
  forgetSystemLookups();

  System* base = ptr;
  detail::EventHost::declaring(*base, true);
  base->create();
  detail::EventHost::declaring(*base, false);
  attach(*base);

  return *ptr;
}

template <typename TService, typename... TArgs>
TService& Registry::addService(TArgs&&... args)
{
  static_assert(std::is_base_of_v<Service, TService>,
                "addService<T>() adds an sfs::Service (docs/core/services); "
                "add a System with addSystem<T>()");
  auto service = std::make_unique<TService>(std::forward<TArgs>(args)...);
  TService* ptr = service.get();
  Service* base = ptr;
  base->m_registry = this;
  detail::EventHost::attach(*base, &m_events);
  m_services.emplace_back(std::move(service));

  detail::EventHost::declaring(*base, true);
  base->create();
  detail::EventHost::declaring(*base, false);
  return *ptr;
}

template <typename Fn>
void Registry::forEachService(Fn&& fn)
{
  for (const std::unique_ptr<Service>& service : m_services)
    fn(*service);
}

template <typename TSystem>
void Registry::removeSystem()
{
  // Forgotten before as well as after: a destructor that looks a system up
  // mustn't be handed the one being destroyed.
  forgetSystemLookups();
  systems.erase(std::remove_if(systems.begin(),
                               systems.end(),
                               [](const std::unique_ptr<System>& system) {
                                 return dynamic_cast<TSystem*>(system.get()) !=
                                        nullptr;
                               }),
                systems.end());
  forgetSystemLookups();
}

template <typename TSystem>
bool Registry::hasSystem() const
{
  return tryGetSystem<TSystem>() != nullptr;
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
  const auto id = static_cast<std::size_t>(detail::systemLookupId<TSystem>());
  if (id >= kSystemSlots)
    return findSystem<TSystem>();

  std::atomic<std::uintptr_t>& slot = m_systemSlots[id];
  const std::uintptr_t known = slot.load(std::memory_order_acquire);
  if (known != kNotLookedUp)
  {
    // Two system types with one name share a slot: the answer may be the
    // other's.
    const std::type_info* foundFor =
        debugFeatures::compiled() && debugFeatures::enabled()
            ? m_systemSlotTypes[id].load(std::memory_order_acquire)
            : nullptr;
    if (foundFor && *foundFor != typeid(TSystem))
    {
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
        LOG_ERROR(
            "tryGetSystem: two system types are named " +
            detail::readableTypeName(detail::componentTypeKey<TSystem>()) +
            "; give them distinct names (looked up by a scan instead)");
      return findSystem<TSystem>();
    }
    return known == kNoSuchSystem ? nullptr : reinterpret_cast<TSystem*>(known);
  }

  // Remembered only if the systems didn't change while they were scanned,
  // and taken back if they changed as it was stored.
  const uint64_t changed = m_systemsChanged.load();
  TSystem* found = findSystem<TSystem>();
  std::uintptr_t answer =
      found ? reinterpret_cast<std::uintptr_t>(found) : kNoSuchSystem;
  if (m_systemsChanged.load() == changed)
  {
    m_systemSlotTypes[id].store(&typeid(TSystem));
    slot.store(answer);
    if (m_systemsChanged.load() != changed)
      slot.compare_exchange_strong(answer, kNotLookedUp);
  }
  return found;
}

template <typename TSystem>
TSystem* Registry::findSystem() const
{
  for (const auto& system : systems)
    if (auto* casted = dynamic_cast<TSystem*>(system.get()))
      return casted;
  return nullptr;
}

template <typename Fn>
void Registry::forEachSystem(Fn&& fn)
{
  for (auto& system : systems)
    fn(*system);
}

} // namespace sfs
