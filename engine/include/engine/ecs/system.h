#pragma once

#include "component.h"
#include "entity.h"

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
