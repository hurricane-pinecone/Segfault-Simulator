#pragma once

#include "component.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/moduleSettings.h"
#include "engine/core/ecs/query.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace sfs
{

namespace detail
{

/**
 * TSystem's lookup id from systemTypeId(). Two system types must not share a
 * qualified name, as two component types must not: the id comes from the
 * name, so they would share one. Local classes of the same name in one
 * function, or anonymous-namespace classes in different files, would.
 * @return its id
 */
template <typename TSystem>
int systemLookupId()
{
  return moduleTypeId<TSystem, &systemTypeId>();
}

} // namespace detail

class System
{
public:
  System() = default;
  virtual ~System() = default;

  /**
   * The entities this system works on: the living, flushed entities that
   * match the terms it registered (registerComponent()), in id order. It is
   * the registry's shared query for that signature (Registry::query()), so
   * the system keeps no copy. Empty until the system is added to a registry.
   * The list changes as entities join and leave: to add or remove these
   * component types while going through it, go through a copy.
   * @return the list, read-only
   */
  const std::vector<Entity>& getEntities() const;

  /** What an entity must match to be one of getEntities(). */
  const QuerySignature& getComponentSignature() const;

  // Disabled systems are skipped by Scene update/render and by render
  // providers. Intended for debug toggling; the render system is never
  // disabled.
  bool enabled() const { return m_enabled; }
  void setEnabled(bool enabled) { m_enabled = enabled; }

  // How the scene's game time (Registry::timeScale()) reaches update().
  // Stepped, the default: game time, in steps no longer than the registry's
  // longest step (several a frame when the game runs fast), so what steers,
  // follows a path or meets other moving things does the same at any speed.
  // Frame: a frame's game time in one step (what only adds time up, or costs
  // too much to run more than once a frame). Real: real time, whatever the
  // time scale (rendering, cameras).
  enum class TimeStep
  {
    Stepped,
    Frame,
    Real
  };
  TimeStep timeStep() const { return m_timeStep; }
  void setTimeStep(TimeStep step) { m_timeStep = step; }

  // UI-agnostic descriptors for a system's debug settings, surfaced by the
  // debug panel the same way render modules expose theirs. Default: none.
  virtual std::vector<ModuleSetting> settings() { return {}; }

protected:
  virtual void create(){};
  virtual void update(double deltatime){};
  /**
   * Runs once a frame after every game object's onUpdate() (Unity's
   * LateUpdate): for what must follow what the objects did, such as a camera
   * kept clear of walls after a script has moved it. A Scene calls it on its
   * enabled systems in the order they were added, after the scene's own
   * onUpdate(). A Real system gets the frame's real time; Stepped and Frame
   * ones the frame's game time, in one step.
   * @param deltaTime the frame's time, as above
   */
  virtual void lateUpdate(double deltaTime){};
  virtual void render(){};

  /**
   * One of this system's entities (getEntities()) joined (onEntityAdded) or
   * left (onEntityRemoved). It joins when it is flushed (a new entity), gains
   * a component, or is already there when the system is added; it leaves when
   * it loses a component or is destroyed (at the flush). Joining, it has all
   * the registered components. Leaving, it is still alive with its other
   * components (a destroyed one still has them all; one that lost a component
   * no longer has that one). For one change, every list is updated first,
   * then the systems are told in registration order and, within a system,
   * these hooks first, then its observe() callbacks in the order they were
   * declared. With an Excluding<...> term, gaining an excluded component
   * makes it leave (it already has that one) and losing it makes it join.
   * Keep them to the system's own state: don't add or remove components,
   * entities or systems from here.
   * @param entity the entity joining or leaving
   */
  virtual void onEntityAdded(const Entity& entity) {}
  /** See onEntityAdded(). */
  virtual void onEntityRemoved(const Entity& entity) {}

  /**
   * Adds a term to what an entity needs to be one of this system's entities
   * (getEntities(), onEntityAdded()), as Registry::query() takes them: a
   * component it must have, an Any<...> group it must have one of (one group
   * per system: a second is logged and ignored) or an Excluding<...> group
   * it must have none of. Call it in the constructor or create(), before the
   * system is added to a registry.
   */
  template <typename TTerm>
  void registerComponent();

  /** A callback observe() calls with the entity joining or leaving. */
  using EntityCallback = std::function<void(const Entity&)>;

  /**
   * Reacts as another kind of entity, those matching TTerms (components and
   * filter groups, as Registry::query() takes them), joins or leaves (set
   * something up for a new wheel, release it as it goes), with the same
   * timing as onEntityAdded() and onEntityRemoved(). Read the list itself
   * with registry->query<TTerms...>(). Call it where the components are
   * registered (the constructor or create()). Only use it where a reaction
   * is needed; to just read a list, query it.
   * @param added called as one joins, e.g.
   *   `[this](const sfs::Entity& wheel) { setUpWheel(wheel); }`
   * @param removed called as one leaves (none: nothing to release)
   */
  template <typename... TTerms>
  void observe(EntityCallback added, EntityCallback removed = {});

private:
  void setRegistry(Registry* registry) { this->registry = registry; }

  // Adds an observer; if the system is already in a registry, it follows it
  // from now on (told of the entities already there).
  void addObserver(const QuerySignature& signature,
                   EntityCallback added,
                   EntityCallback removed);
  // A registerComponent() after the system was added to a registry: logged,
  // as it can't change the list it already follows.
  void warnLateRegister() const;
  // A registerComponent() of a second Any<...> group (its type key, from
  // detail::componentTypeKey()): logged and ignored.
  void warnSecondAny(std::string_view groupKey) const;

  // How many lists it follows (its entities, then each observer's) and the
  // signature of list `index` (0: the registered entities, k: observer k-1).
  std::size_t listCount() const { return 1 + m_observers.size(); }
  const QuerySignature& signatureOf(std::size_t index) const;
  // List `index` gained or lost an entity: the hook or callback for it.
  void tellJoined(std::size_t index, const Entity& entity);
  void tellLeft(std::size_t index, const Entity& entity);

protected:
  Registry* registry = nullptr;

private:
  struct Observer
  {
    QuerySignature signature;
    EntityCallback added;
    EntityCallback removed;
  };
  QuerySignature m_signature;        // registerComponent()'s
  std::vector<Observer> m_observers; // in declaration order
  // The registry's query list for m_signature, then for each observer, set
  // when the system is added to a registry.
  std::vector<const std::vector<Entity>*> m_lists;
  bool m_enabled = true;
  TimeStep m_timeStep = TimeStep::Stepped;

  friend class Registry;
  friend class Scene;
};

template <typename TTerm>
void System::registerComponent()
{
  if (!m_lists.empty())
  {
    warnLateRegister();
    return;
  }
  if (detail::QueryTerm<TTerm>::isAny && m_signature.any.any())
  {
    warnSecondAny(detail::componentTypeKey<TTerm>());
    return;
  }
  detail::QueryTerm<TTerm>::addTo(m_signature);
}

template <typename... TTerms>
void System::observe(EntityCallback added, EntityCallback removed)
{
  addObserver(detail::makeQuerySignature<TTerms...>(),
              std::move(added),
              std::move(removed));
}

} // namespace sfs
