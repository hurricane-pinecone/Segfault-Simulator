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
#include "engine/core/ecs/hierarchyLinks.h"
#include "pool.h"
#include "query.h"
#include "system.h"
#include <engine/core/logger/logger.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace sfs
{

namespace hierarchy
{
bool setParent(Entity child, const Entity& parent);
}

class Registry
{
public:
  Registry() = default;
  /**
   * Destroys the systems first, while every entity, component and query
   * still exists, so a system's destructor can read the world it leaves
   * (as Unity's OnDestroy and Unreal's EndPlay do). The rest goes after.
   */
  ~Registry();

  Entity createEntity();
  Entity getEntity(Entity::EntityId id);
  void destroyEntity(const Entity& entity);
  bool isAlive(const Entity& entity) const;

  /** Every living entity, in id order. */
  std::vector<Entity> entities();

  void flushEntities();

  /**
   * The parent/child links of the entities, by id (see hierarchy.h, which
   * reads them). Only hierarchy::setParent and the flush that removes an
   * entity change them.
   */
  const HierarchyLinks& links() const { return m_links; }

  /**
   * Every living entity, parents before children, roots and siblings in id
   * order (HierarchyLinks::preorder). It's rebuilt first if a link changed or
   * an entity was created or removed since it was last built: O(entities)
   * then, and a read otherwise. The first call after such a change rewrites
   * it in place, so while iterating it, call nothing that may call
   * preorder() (a save, findEntity). Main thread only.
   * @return the index
   */
  const std::vector<HierarchyLinks::Placed>& preorder();

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

  /** Removes and destroys every system that is a TSystem. */
  template <typename TSystem>
  void removeSystem();

  /**
   * Whether a system that is a TSystem, of that type or one derived from it,
   * is in the registry.
   * @return true if tryGetSystem<TSystem>() finds one
   */
  template <typename TSystem>
  bool hasSystem() const;

  /**
   * The system that is a TSystem, as tryGetSystem() finds it. One missing is
   * a fatal error naming the type: use tryGetSystem() where it may be absent.
   * @return the system
   */
  template <typename TSystem>
  TSystem& getSystem() const;

  /**
   * The first system, in the order they were added, that is a TSystem: of
   * that type or one derived from it. The answer is remembered by type until a
   * system is added or removed, so a lookup costs the same whatever the
   * number of systems. While the registry is being destroyed there are none.
   *
   * Any thread may look systems up, but systems are added and removed only
   * while no other thread is looking one up: the lookup reads the list of
   * systems. A lookup that overlaps a change anyway remembers nothing.
   * System types must have distinct qualified names (see
   * detail::systemLookupId()); debug builds log a clash.
   * @return the system, or null if there is none
   */
  template <typename TSystem>
  TSystem* tryGetSystem() const;

  template <typename Fn>
  void forEachSystem(Fn&& fn);

  /**
   * Game time: how fast the systems' time runs against real time (2: twice
   * as fast, 0: stopped; see System::TimeStep). Scene::update() reads it once
   * a frame, so a change made during one takes effect from the next.
   */
  void setTimeScale(double scale) { m_timeScale = scale > 0.0 ? scale : 0.0; }
  double timeScale() const { return m_timeScale; }

  /**
   * The longest step a Stepped system takes (0, the default: a frame's game
   * time in one step), and the most steps a frame: past that many the game
   * falls behind (runs slower than its time scale) rather than take longer
   * steps, so a slow frame can't make the next one slower still.
   */
  void setMaxTimeStep(double seconds, int mostSteps = 16)
  {
    m_maxTimeStep = seconds > 0.0 ? seconds : 0.0;
    m_mostTimeSteps = mostSteps > 1 ? mostSteps : 1;
  }
  double maxTimeStep() const { return m_maxTimeStep; }
  int mostTimeSteps() const { return m_mostTimeSteps; }

  /**
   * Game seconds since this registry began updating: the sum of the game
   * time its systems have been given. A Stepped system reads the time at the
   * end of the step it is given; Frame and Real systems read the frame's end.
   */
  double gameTime() const { return m_gameTime; }

  /**
   * Whether the game is paused. The engine's own moving parts (animations,
   * particles) stand still while it is. Systems keep the game time the time
   * scale gives them, and each decides what a pause stops for it by reading
   * paused(). Like the time scale, it takes effect from the next frame.
   */
  void setPaused(bool paused) { m_paused = paused; }
  bool paused() const { return m_paused; }

  /**
   * Goes up whenever an entity gains or loses a TComponent (one destroyed
   * included), so a system can keep a list of the entities that have one and
   * look again only when this has moved. Changing a component's values
   * doesn't count.
   */
  template <typename TComponent>
  uint64_t componentChanges() const;

  /**
   * The registry's one TComponent, for components a scene has exactly one of
   * (a world clock, the weather): constant time, kept up to date as
   * components are added and removed. Null when no living entity has one.
   * When several do, it logs a warning (once per type) and returns one.
   */
  template <typename TComponent>
  TComponent* singleton();

  /**
   * The entities that match these terms, in id order: the living ones that
   * have been flushed (a new entity joins at the next flushEntities(), as it
   * joins systems; a destroyed one leaves at the flush). A term is a
   * component the entity must have, one Any<...> group it must have at least
   * one of, or an Excluding<...> group it must have none of (it wins over
   * the others), e.g. query<TransformComponent, Any<A, B>, Excluding<C>>().
   * The registry makes the list the first time it's asked for (one scan) and
   * keeps it up to date from then on as components are added and removed,
   * so asking again, every frame or anywhere else, costs nothing, and
   * everyone asking for the same terms shares it (in any order, and terms
   * that always list the same entities count as the same: Any<A> is A). The
   * reference stays valid as long as the registry. To add or remove these
   * component types while going through it, go through a copy.
   * @return the shared list, read-only
   */
  template <typename... TTerms>
  const std::vector<Entity>& query();

private:
  // A query list an entity just joined (or left), for the hooks.
  struct QueryChange
  {
    const std::vector<Entity>* list = nullptr;
    bool joined = false;
  };
  // The index of the list for a canonical signature, made and filled the
  // first time.
  uint32_t listOf(const QuerySignature& signature);
  // Whether an entity is waiting for its first flush (in no query yet).
  bool isPending(Entity::EntityId id) const;
  // An entity's components went from `before` to `after`, and it was (`wasIn`)
  // and now is (`isIn`) a flushed, living entity: it joins or leaves the
  // queries that now match it, then the systems following them are told.
  // Only the lists by index in `lists` are checked (null: every list).
  void syncQueries(Entity::EntityId id,
                   const Signature& before,
                   bool wasIn,
                   const Signature& after,
                   bool isIn,
                   const std::vector<uint32_t>* lists = nullptr);
  // A system just added (after its create()): it follows the queries of its
  // entities and observers, and is told of their members, entity by entity in
  // id order.
  void attach(System& system);
  // A system already in the registry follows its list `index` too (an
  // observe() after it was added), told of the members already there.
  void follow(System& system, std::size_t index);

  // An entity gained or lost the one component `componentId`, its
  // components being `before` until then: it joins or leaves the queries
  // that now match it (one waiting for a flush joins them then).
  void componentsChanged(const Entity& entity,
                         const Signature& before,
                         std::size_t componentId);
  // Counts a component an entity gained or lost as changed, and keeps
  // singleton()'s holders up to date (pending entities included).
  void countChange(Entity::EntityId id, std::size_t componentId, bool gained);
  // countChange() for every component that differs between two signatures.
  void countChanges(Entity::EntityId id,
                    const Signature& before,
                    const Signature& after);
  // The id of a living entity with this component, found by a scan (when
  // the remembered holder lost it while others still have one).
  Entity::EntityId findHolder(std::size_t componentId) const;
  void warnNotSingle(std::size_t componentId, std::string_view type);

  // A component accessor used on an entity it can't be: fatal (see
  // fatalError()), naming what was asked for and why.
  [[noreturn]] void badComponentAccess(const Entity& entity,
                                       const char* action,
                                       std::string_view type) const;

  template <typename TComponent>
  Pool<TComponent>* poolOf();

  // Puts `child` under `parent` (HierarchyLinks::kNone: makes it a root), in
  // id order among its new siblings. Both are living ids, and the caller has
  // refused loops (hierarchy::setParent, the one caller).
  void relink(Entity::EntityId child, Entity::EntityId parent);
  // Takes `id` out of its parent's children, leaving it a root.
  void unlink(Entity::EntityId id);
  // A removed entity's links go: it leaves its parent, and its children
  // become roots.
  void unlinkRemoved(Entity::EntityId id);

private:
  friend class Scene;
  friend class System;
  friend bool hierarchy::setParent(Entity child, const Entity& parent);

  // Scene adds each step's game time before that step's systems run.
  void advanceGameTime(double seconds) { m_gameTime += seconds; }

  uint32_t entityCount = 0;
  uint64_t nextDebugId = 1;

  // Recycled lowest-first, so recreating a cleared set of entities (a scene
  // reload, an undo) reproduces their ids and order exactly, whatever order
  // they were freed in.
  std::set<uint32_t> freeEntityIds;
  std::vector<uint64_t> entityDebugIds;
  std::vector<uint32_t> generations;
  std::vector<bool> entityAlive;
  // The hierarchy, its nodes sized with the arrays above.
  HierarchyLinks m_links;
  // Goes up when an entity is created or removed: with the links' version,
  // what tells whether the preorder index is out of date.
  uint64_t m_entitySetVersion = 0;
  std::set<Entity> entitiesToBeAdded;
  std::set<Entity> entitiesToBeRemoved;

  // vector index = component type id
  // ComponentPool index = entity id
  std::vector<std::unique_ptr<IPool>> componentPools;

  // Vector of component signatures, which allows us to tell which components
  // are turned on per entity
  std::vector<Signature> entityComponentSignatures;

  std::vector<std::unique_ptr<System>> systems;

  // tryGetSystem()'s answers, by detail::systemLookupId(): the system found
  // for that type, already cast to it, kNoSuchSystem, or kNotLookedUp since
  // the systems last changed; and the type each was found for, to check a
  // remembered answer against in debug builds. They are atomic, so lookups
  // from other threads are safe while the systems don't change. A type whose
  // id is past the slots is looked up by a scan every time.
  static constexpr std::size_t kSystemSlots = 256;
  static constexpr std::uintptr_t kNotLookedUp = 0;
  static constexpr std::uintptr_t kNoSuchSystem = 1;
  mutable std::array<std::atomic<std::uintptr_t>, kSystemSlots> m_systemSlots{};
  mutable std::array<std::atomic<const std::type_info*>, kSystemSlots>
      m_systemSlotTypes{};
  // Goes up each time the systems change, so a lookup that scanned while they
  // did leaves no answer behind.
  std::atomic<uint64_t> m_systemsChanged{0};
  // The systems changed: every type is looked up again on its next ask.
  void forgetSystemLookups();
  // The first system that is a TSystem, in the order they were added; null if
  // none is.
  template <typename TSystem>
  TSystem* findSystem() const;

  // By component id: how often an entity gained or lost one.
  std::array<uint64_t, MAX_COMPONENTS> m_componentChanges{};
  // By component id: how many entities have one, one of them (kNoHolder:
  // to be found again), and whether singleton() has warned about several.
  static constexpr Entity::EntityId kNoHolder = ~Entity::EntityId{0};
  std::array<uint32_t, MAX_COMPONENTS> m_holderCount{};
  std::array<Entity::EntityId, MAX_COMPONENTS> m_holder = []
  {
    std::array<Entity::EntityId, MAX_COMPONENTS> holders{};
    holders.fill(kNoHolder);
    return holders;
  }();
  std::array<bool, MAX_COMPONENTS> m_warnedNotSingle{};
  // query()'s lists, by index: each one's canonical signature (dense, for a
  // flush to test them all), its entities in id order (a deque, so a list's
  // references stay valid as more are made) and whether a system has
  // followed it (never cleared: a change to lists no system follows tells
  // no system).
  std::vector<QuerySignature> m_querySignatures;
  std::deque<std::vector<Entity>> m_queryLists;
  std::vector<uint8_t> m_queryFollowed;
  // The index of each signature's list.
  std::unordered_map<QuerySignature, uint32_t> m_queryIndex;
  // By component id: the lists whose signature names it (in all, any or
  // excluding), by index. Only those can change as an entity gains or loses
  // one.
  std::array<std::vector<uint32_t>, MAX_COMPONENTS> m_queriesNaming;
  // syncQueries()'s scratch, reused between calls.
  std::vector<QueryChange> m_queryChanges;

  double m_timeScale = 1.0;
  double m_maxTimeStep = 0.0;
  int m_mostTimeSteps = 16;
  double m_gameTime = 0.0;
  bool m_paused = false;
};

} // namespace sfs

#include "entity.inl"   // IWYU pragma: keep
#include "registry.inl" // IWYU pragma: keep
