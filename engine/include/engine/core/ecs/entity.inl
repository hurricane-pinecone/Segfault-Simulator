#pragma once

#include "registry.h"

namespace sfs
{

namespace detail
{
// An entity handle with no registry: default-made, never an entity.
[[noreturn]] void invalidEntity(const char* action);
} // namespace detail

template <typename TComponent, typename... TArgs>
Entity& Entity::addComponent(TArgs&&... args)
{
  if (!registry)
    detail::invalidEntity("addComponent");
  registry->addComponent<TComponent>(*this, std::forward<TArgs>(args)...);
  return *this;
}

template <typename TComponent, typename... TArgs>
Entity& Entity::addTag(TArgs&&... args)
{
  if (!registry)
    detail::invalidEntity("addTag");
  registry->addComponent<TComponent>(*this, std::forward<TArgs>(args)...);
  return *this;
}

template <typename TComponent>
void Entity::removeComponent() const
{
  if (registry)
    registry->removeComponent<TComponent>(*this);
};

template <typename TComponent>
bool Entity::hasComponent() const
{
  return registry && registry->hasComponent<TComponent>(*this);
};

template <typename TComponent>
TComponent& Entity::getComponent() const
{
  if (!registry)
    detail::invalidEntity("getComponent");
  return registry->getComponent<TComponent>(*this);
}

} // namespace sfs
