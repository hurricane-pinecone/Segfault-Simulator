#pragma once

#include "registry.h"

namespace sfs
{

inline bool Entity::isValid() const
{
  return registry && registry->isValid(*this);
}

template <typename TComponent, typename... TArgs>
Entity& Entity::addComponent(TArgs&&... args)
{
  if (!registry)
    Registry::refuseAdd(
        *this, "addComponent", detail::componentTypeKey<TComponent>());
  else
    registry->add<TComponent>(
        "addComponent", *this, std::forward<TArgs>(args)...);
  return *this;
}

template <typename TComponent, typename... TArgs>
Entity& Entity::addTag(TArgs&&... args)
{
  if (!registry)
    Registry::refuseAdd(
        *this, "addTag", detail::componentTypeKey<TComponent>());
  else
    registry->add<TComponent>("addTag", *this, std::forward<TArgs>(args)...);
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

template <typename... TTerms>
bool Entity::matches() const
{
  return isValid() &&
         registry->matches(id, detail::querySignature<TTerms...>());
}

template <typename TComponent>
TComponent* Entity::getComponent() const
{
  return registry ? registry->getComponent<TComponent>(*this) : nullptr;
}

} // namespace sfs
