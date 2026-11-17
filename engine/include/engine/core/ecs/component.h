#pragma once

#include "engine/core/ecs/sceneData.h"
#include "engine/core/util/typeKey.h"

#include <string_view>
#include <type_traits>

namespace sfs
{

namespace detail
{

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

} // namespace detail

template <typename T>
class Component
{
public:
  /**
   * The id of component T, shared by every entity and every module: entity1's
   * and entity2's TransformComponent both have id 1, say. A const T is T
   * (`getComponent<const T>()`, a `const T` query term), with T's id. A
   * type marked [[sfs::scene_data]] isn't a component: it fails to compile.
   * @return T's id
   */
  static int getId()
  {
    (void)sizeof(detail::NotAComponent<std::remove_cv_t<T>>);
    return detail::moduleTypeId<std::remove_cv_t<T>,
                                &detail::componentTypeId>();
  }
};

} // namespace sfs
