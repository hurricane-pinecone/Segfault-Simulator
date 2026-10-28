#pragma once

#include "engine/core/assets/assetDatabase.h"
#include "engine/core/ecs/entity.h"
#include "glm/glm/ext/vector_float3.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

struct EditorContext;

// The editor's prefab operations: making a prefab from part of the scene,
// placing instances, and pushing an instance's changes to its prefab or
// dropping them. Each scene change is one undoable edit (writing a prefab file
// isn't undone).
namespace prefabs
{

/** "asset:<id>": how scenes name a prefab. */
std::string nameOf(const sfs::AssetId& id);

/** The prefab asset a name means; nothing if it isn't one in the database. */
const sfs::AssetRecord* recordOf(const EditorContext& context, const std::string& name);

/** A prefab's name for people: its path in assets/, or the name itself. */
std::string label(const EditorContext& context, const std::string& name);

/**
 * Save an entity and everything below it as a new prefab in `folder`
 * (relative to the project, e.g. "prefabs"), named after the entity; the entity becomes an
 * instance of it.
 *
 * @return the prefab's id, or nothing (logged)
 */
std::optional<sfs::AssetId> create(EditorContext& context, sfs::Entity entity, const std::filesystem::path& folder);

/**
 * Place an instance under `parent` (none: a root), at `position` if given
 * (else where the prefab puts it), and select it.
 *
 * @return its root
 */
sfs::Entity place(EditorContext& context,
                  const std::string& prefab,
                  const sfs::Entity& parent,
                  std::optional<glm::vec3> position = std::nullopt);

/** Write an instance's changes into its prefab; every instance updates. */
void apply(EditorContext& context, const sfs::Entity& root);

/** Drop an instance's changes, except where it's placed. */
void revert(EditorContext& context, const sfs::Entity& root);

/** Drop one overridden field (or, with field empty, a whole added component). */
void revertField(EditorContext& context, const sfs::Entity& entity, const std::string& component, const std::string& field);

/**
 * What an instance overrides on one of its entities: component -> field
 * names (an empty set: the whole component is the instance's own). Empty if
 * the entity isn't part of an instance.
 */
std::map<std::string, std::set<std::string>> overridesOf(EditorContext& context, const sfs::Entity& entity);

} // namespace prefabs
