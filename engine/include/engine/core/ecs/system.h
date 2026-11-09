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

  // The entities it works on, in no particular order. Iterate them; don't
  // add, remove or reorder them (the registry keeps the list).
  const std::vector<Entity>& getEntities() const;
  std::vector<Entity>& getEntities();
  const Signature& getComponentSignature() const;

  void addEntity(const Entity& entity);
  void removeEntity(const Entity& entity);

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

  template <typename TComponent>
  void registerComponent();

private:
  void setRegistry(Registry* registry) { this->registry = registry; }

protected:
  Registry* registry = nullptr;

private:
  Signature componentSignature;
  std::vector<Entity> entities;
  // Each entity's place in `entities`, by id (kNowhere: not there), so one
  // leaves in constant time.
  static constexpr uint32_t kNowhere = 0xFFFFFFFFu;
  std::vector<uint32_t> m_positions;
  bool m_enabled = true;
  TimeStep m_timeStep = TimeStep::Stepped;

  friend class Registry;
  friend class Scene;
};

template <typename TComponent>
void System::registerComponent()
{
  const auto componentId = Component<TComponent>::getId();
  componentSignature.set(componentId);
}

} // namespace sfs
