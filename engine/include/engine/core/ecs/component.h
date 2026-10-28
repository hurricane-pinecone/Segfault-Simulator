#pragma once

#include "engine/core/ecs/entity.h"

#include <string_view>

namespace sfs
{

namespace detail
{

/**
 * A name for T that is identical in every module built by the same compiler:
 * the compiler's own spelling of this function's signature, which embeds T.
 */
template <typename T>
constexpr std::string_view componentTypeKey()
{
#if defined(_MSC_VER) && !defined(__clang__)
  return __FUNCSIG__;
#else
  return __PRETTY_FUNCTION__;
#endif
}

/**
 * The component id for a type key, assigning the next free id on first use.
 * Defined once, in engine-core, so the engine, an editor and a game module
 * loaded at runtime all agree on every component's id; a per-module counter
 * would number the same type differently in each.
 */
int componentTypeId(std::string_view typeKey);

} // namespace detail

template <typename T>
class Component
{
public:
  // Unique id of component T, shared by every entity and every module.
  // IE: entity1 TransformComponent::getId() -> 1
  //     entity2 TransformComponent::getId() -> 1
  static int getId()
  {
    // Cached per module; the registry behind it is shared.
    static const int id =
        detail::componentTypeId(detail::componentTypeKey<T>());
    return id;
  }
};

} // namespace sfs
