#pragma once

// Registry owns all ECS state: entity ids/generations, component pools, and the
// systems. It is directly constructable, so engine-core can drive the ECS
// headless (tools, tests, or a host that brings its own rendering) with no
// dependency on the runtime.
//
// In a Scene-based game, code does not handle the Registry directly: entities
// are spawned and queried through Scene (createEntity, addSystem, getSystem, ...)
// and components are mutated through Entity (addComponent, ...); a System
// subclass reaches it through the protected `registry` pointer it inherits.
// Scene owns a Registry by value and keeps it private, so that routing holds
// even though this header is visible to client translation units: Entity/Scene
// expose template methods (addComponent<T>, addSystem<T>) that clients
// instantiate over their own types, so the definition must be visible wherever
// those instantiations happen.

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
  Registry() = default;
  ~Registry() = default;

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
  void addEntityToSystems(const Entity& entity);
  void removeEntityFromSystems(const Entity& entity);

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
