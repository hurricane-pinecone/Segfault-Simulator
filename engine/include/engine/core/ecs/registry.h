#pragma once

// Registry owns all ECS state: entity ids/generations, component pools, and the
// systems. It is directly constructable, so engine-core can drive the ECS
// headless (tools, tests, or a host that brings its own rendering) with no
// dependency on the runtime.
//
// In a Scene-based game, code does not handle the Registry directly: entities
// are spawned and queried through Scene (createEntity, addSystem, getSystem,
// ...) and components are mutated through Entity (addComponent, ...); a System
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
#include <string_view>
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

  /** Every living entity, in id order (e.g. for an editor's hierarchy). */
  std::vector<Entity> entities();

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

  /**
   * Call `fn(entity, components&...)` for every living entity with all these
   * components, in id order: view() without making a list. `fn` mustn't add
   * or remove these component types (creating and destroying entities is
   * fine: that waits for the next flush).
   */
  template <typename... TComponents, typename Fn>
  void each(Fn&& fn);

  template <typename Fn>
  void forEachSystem(Fn&& fn);

private:
  void addEntityToSystems(const Entity& entity);

  // An entity's components changed from `before`: it joins or leaves the
  // systems that now match it (one waiting for a flush joins them then).
  void componentsChanged(const Entity& entity, const Signature& before);

  // A component accessor used on an entity it can't be: fatal (see
  // fatalError()), naming what was asked for and why.
  [[noreturn]] void badComponentAccess(const Entity& entity,
                                       const char* action,
                                       std::string_view type) const;

  template <typename TComponent>
  Pool<TComponent>* poolOf();

  // Give a newly added system the living entities that already match it
  // (entities still pending a flush join it at that flush instead).
  void addExistingEntities(System& system);
  void removeEntityFromSystems(const Entity& entity);

private:
  uint32_t entityCount = 0;
  uint64_t nextDebugId = 1;

  // Recycled lowest-first, so recreating a cleared set of entities (a scene
  // reload, an undo) reproduces their ids and order exactly, whatever order
  // they were freed in.
  std::set<uint32_t> freeEntityIds;
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
