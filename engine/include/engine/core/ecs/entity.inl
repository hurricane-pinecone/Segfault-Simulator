#pragma once

#include "registry.h"

namespace sfs
{

template <typename TComponent, typename... TArgs>
Entity& Entity::addComponent(TArgs&&... args)
{
  registry->addComponent<TComponent>(*this, std::forward<TArgs>(args)...);
  return *this;
}

template <typename TComponent, typename... TArgs>
Entity& Entity::addTag(TArgs&&... args)
{
  registry->addComponent<TComponent>(*this, std::forward<TArgs>(args)...);
  return *this;
}

template <typename TComponent>
void Entity::removeComponent() const
{
  registry->removeComponent<TComponent>(*this);
};

template <typename TComponent>
bool Entity::hasComponent() const
{
  return registry->hasComponent<TComponent>(*this);
};

template <typename TComponent>
TComponent& Entity::getComponent() const
{
  return registry->getComponent<TComponent>(*this);
}

} // namespace sfs
