#pragma once

// Internal engine header — not part of the public client API.
//
// Registry is the sole owner of all ECS state. Game code never reaches for it
// directly: entities are spawned and queried through Scene (createEntity,
// addSystem, getSystem, ...) and components are mutated through Entity
// (addComponent, ...). The one place that uses Registry's interface directly is
// a System subclass, through the protected `registry` pointer it inherits —
// that is the supported extension point for system authors.
//
// This header cannot be hidden from client translation units: Scene holds a
// Registry by value, and Entity/Scene expose template methods (addComponent<T>,
// addSystem<T>) that clients instantiate over their own types, so the
// definition must be visible wherever those instantiations happen. The boundary
// is therefore enforced by access control (private ctor, friended Scene), not
// by the include graph. Prefer including the gateway header below.
//
// IWYU pragma: private, include "engine/runtime/sceneManager/scene.h"

#include "engine/core/ecs/entity.h"
#include "pool.h"
#include "system.h"
#include <engine/core/logger/logger.h>

#include <memory>
#include <set>
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
  // Only Scene constructs a Registry; everything else goes through Scene. See
  // the header note above on why the type is still visible to client TUs.
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

} // namespace sfs

#include "entity.inl"   // IWYU pragma: keep
#include "registry.inl" // IWYU pragma: keep
