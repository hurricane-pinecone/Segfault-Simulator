#pragma once

#include "component.h"
#include "entity.h"
#include "signature.h"

#include <vector>

class System
{
public:
  System() = default;
  virtual ~System() = default;

  const std::vector<Entity>& getEntities() const;
  const Signature& getComponentSignature() const;

  virtual void update(double deltatime) {};
  void addEntity(const Entity& entity);
  void removeEntity(const Entity& entity);

  template <typename TComponent>
  void registerComponent();

private:
  Signature componentSignature;
  std::vector<Entity> entities;
};

template <typename TComponent>
void System::registerComponent()
{
  const auto componentId = Component<TComponent>::getId();
  componentSignature.set(componentId);
}
