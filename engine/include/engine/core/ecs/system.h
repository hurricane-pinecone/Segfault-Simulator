#pragma once

#include "component.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/moduleSettings.h"

#include <cstdint>
#include <vector>

namespace sfs
{

class System
{
public:
  System() = default;
  virtual ~System() = default;

  // The entities it works on (those with every component it registered),
  // in no particular order. Iterate them; don't add, remove or reorder them
  // (the registry keeps the list).
  const std::vector<Entity>& getEntities() const;
  std::vector<Entity>& getEntities();
  // The entities of another of its sets (watch(); 0: the ones above).
  const std::vector<Entity>& getEntities(int set) const;
  const Signature& getComponentSignature() const;

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
  virtual void render(){};

  // An entity joined or left one of its sets (0: its registered components'
  // set; others: watch()'s), as it gained or lost components, was created
  // or destroyed, or the system was added to a registry that already had
  // it. Joining, it has all the set's components. Leaving, it's out of the
  // set but still alive, with its other components (a destroyed one has
  // them all; one that lost a component no longer has that one). Keep them
  // to the system's own state: don't add or remove components, entities or
  // systems from here.
  virtual void onEntityAdded(const Entity& entity, int set) {}
  virtual void onEntityRemoved(const Entity& entity, int set) {}

  template <typename TComponent>
  void registerComponent();

  // Another set of entities it keeps: those with every one of TComponents
  // (getEntities(set), onEntityAdded(..., set)), for a system that also
  // works on a second kind of entity. Call it where its components are
  // registered (its constructor or create()). @return the set's number
  template <typename... TComponents>
  int watch();

private:
  void setRegistry(Registry* registry) { this->registry = registry; }

  // The entity's components are now `components` (alive: false, it's being
  // destroyed): it joins or leaves each of its sets accordingly (Registry).
  void sync(const Entity& entity, const Signature& components, bool alive);

protected:
  Registry* registry = nullptr;

private:
  // A set of entities it keeps: those with every component in `signature`.
  struct Set
  {
    Signature signature;
    std::vector<Entity> entities;
    // Each entity's place in `entities`, by id (kNowhere: not there), so one
    // leaves in constant time.
    std::vector<uint32_t> positions;
  };
  static constexpr uint32_t kNowhere = 0xFFFFFFFFu;
  bool contains(const Set& set, const Entity& entity) const;
  void add(Set& set, const Entity& entity);
  void remove(Set& set, const Entity& entity);

  std::vector<Set> m_sets = std::vector<Set>(1); // [0]: registerComponent()'s
  bool m_enabled = true;
  TimeStep m_timeStep = TimeStep::Stepped;

  friend class Registry;
  friend class Scene;
};

template <typename TComponent>
void System::registerComponent()
{
  const auto componentId = Component<TComponent>::getId();
  m_sets.front().signature.set(componentId);
}

template <typename... TComponents>
int System::watch()
{
  Set set;
  (set.signature.set(Component<TComponents>::getId()), ...);
  m_sets.push_back(std::move(set));
  return static_cast<int>(m_sets.size()) - 1;
}

} // namespace sfs
