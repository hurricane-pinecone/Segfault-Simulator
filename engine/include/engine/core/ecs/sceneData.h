#pragma once

#include "engine/core/util/typeKey.h"

#include <cstddef>
#include <string_view>
#include <type_traits>

// Scene data (docs/core/ecs): plain data types marked
// `struct [[sfs::scene_data]] Scoreboard`, of which a scene holds at most one
// each, apart from every entity. sfs-codegen reads the attribute and writes a
// SceneDataTraits specialization for each marked type into the traits header
// every file of the type's target includes first
// (engine/docs/core/scene-data).
namespace sfs::detail
{

/**
 * What the compiler knows of a type as scene data. This is a type that isn't
 * marked; sfs-codegen specialises it for each marked type.
 */
template <typename T>
struct SceneDataTraits
{
  /** Whether it's marked `[[sfs::scene_data]]`. */
  static constexpr bool marked = false;
};

/** Whether a type is marked `[[sfs::scene_data]]`, const or not. */
template <typename T>
inline constexpr bool kSceneData = SceneDataTraits<std::remove_cv_t<T>>::marked;

/**
 * The scene data slot id for a type key, assigning the next free id on first
 * use, from a table of its own. Defined once, in engine-core, so the engine, an
 * editor and a game module loaded at runtime agree on every type's id.
 * @param typeKey the type's key, componentTypeKey<T>()
 * @return its id, counting from 0
 */
int sceneDataTypeId(std::string_view typeKey);

/**
 * T's scene data slot id; a const T's is T's. Looked up once per module.
 * @return the id
 */
template <typename T>
std::size_t sceneDataId()
{
  return static_cast<std::size_t>(
      moduleTypeId<std::remove_cv_t<T>, &sceneDataTypeId>());
}

/**
 * Fails to compile, naming a type used as scene data that isn't marked
 * (RequireSceneData<Type>), or passes for one that is.
 */
template <typename T>
struct RequireSceneData
{
  static_assert(kSceneData<T>,
                "sceneData<T>(), addSceneData<T>() and removeSceneData<T>() "
                "take a type marked [[sfs::scene_data]] (struct "
                "[[sfs::scene_data]] Scoreboard {...}): data the scene holds "
                "one of. A component goes on entities and is read through a "
                "query");
};

/**
 * Fails to compile, naming a scene data type used as a component
 * (NotAComponent<Type>), or passes for a type that isn't marked.
 */
template <typename T>
struct NotAComponent
{
  static_assert(!kSceneData<T>,
                "This type (NotAComponent<Type>) is marked "
                "[[sfs::scene_data]]: data the scene holds, not a component on "
                "an entity. Read it with sceneData<T>() and add it with "
                "addSceneData<T>()");
};

} // namespace sfs::detail

// The engine's own scene data types' traits, made by sfs-codegen.
#if __has_include("engine/generated/engineEventTraits.gen.h")
  #include "engine/generated/engineEventTraits.gen.h" // IWYU pragma: export
#endif
