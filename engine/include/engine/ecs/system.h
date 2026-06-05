#pragma once

#include "component.h"
#include "engine/ecs/entity.h"
#include "engine/ecs/moduleSettings.h"

#include <vector>

namespace sfs
{

class System
{
public:
  System() = default;
  virtual ~System() = default;

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

  // UI-agnostic descriptors for a system's debug settings, surfaced by the
  // debug panel the same way render modules expose theirs. Default: none.
  virtual std::vector<ModuleSetting> settings() { return {}; }

protected:
  virtual void create() {};
  virtual void update(double deltatime) {};
  virtual void render() {};

  template <typename TComponent>
  void registerComponent();

private:
  void setRegistry(Registry* registry) { this->registry = registry; }

protected:
  Registry* registry = nullptr;

private:
  Signature componentSignature;
  std::vector<Entity> entities;
  bool m_enabled = true;

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
