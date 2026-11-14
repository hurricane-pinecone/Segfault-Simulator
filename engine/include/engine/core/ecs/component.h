#pragma once

#include "engine/core/ecs/entity.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>

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
 * The type a type key names, for people ("Health", "farm::Seed"); the whole
 * key if it can't be picked out.
 */
std::string readableTypeName(std::string_view typeKey);

/**
 * A run-time type's name, for people: demangled where the compiler mangles
 * it ("farm::HarvestSystem"), else as the compiler gives it.
 * @param type the type, e.g. typeid(*this) for an object's own type
 * @return its name
 */
std::string readableTypeName(const std::type_info& type);

/**
 * The component id for a type key, assigning the next free id on first use.
 * Defined once, in engine-core, so the engine, an editor and a game module
 * loaded at runtime all agree on every component's id; a per-module counter
 * would number the same type differently in each.
 *
 * A type past MAX_COMPONENTS (there's no bit for it in a Signature) is a fatal
 * error (see fatalError()), raised every time it's asked for.
 */
int componentTypeId(std::string_view typeKey);

/**
 * The lookup id for a system type's key, assigning the next free id on first
 * use, from a table of its own, separate from the components'. Defined once,
 * in engine-core, like componentTypeId(). Any type a system is looked up by
 * gets one, base classes included.
 * @param typeKey the type's key, componentTypeKey<T>()
 * @return its id, counting from 0
 */
int systemTypeId(std::string_view typeKey);

/**
 * T's id from `idOf` (componentTypeId or systemTypeId), asked for the first
 * time and then remembered in the calling module. The table behind `idOf` is
 * shared, so every module gets the same id. If `idOf` raises a fatal error,
 * nothing is remembered and the next call asks again.
 * @return T's id
 */
template <typename T, int (*idOf)(std::string_view)>
int moduleTypeId()
{
  static const int id = idOf(componentTypeKey<T>());
  return id;
}

} // namespace detail

template <typename T>
class Component
{
public:
  /**
   * The id of component T, shared by every entity and every module: entity1's
   * and entity2's TransformComponent both have id 1, say. A const T is T
   * (`getComponent<const T>()`, a `const T` query term), with T's id.
   * @return T's id
   */
  static int getId()
  {
    return detail::moduleTypeId<std::remove_cv_t<T>,
                                &detail::componentTypeId>();
  }
};

} // namespace sfs
