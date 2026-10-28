#pragma once

#include "engine/core/ecs/entity.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sfs
{

class MeshLibrary;
class Scene;

namespace hierarchy
{
class EntityTree;
}

namespace reflect
{
class TypeRegistry;
struct TypeInfo;
}

/**
 * Saves and loads a scene as JSON, driven by reflection: every reflected
 * component on every entity is written field by field, and read back by type
 * and field name. A component type or field the registry doesn't know (renamed,
 * removed, or from a module that isn't loaded) is reported and skipped, so a
 * scene still loads after its code changes. Meshes are stored by their
 * MeshLibrary name.
 *
 * When the scene has a RenderSystem3D, its lighting, shadow and camera settings
 * are saved and restored too.
 *
 * The scene's game systems (the System scripts it runs when it plays) are
 * saved by name; the caller keeps that list, since edit-mode scenes don't
 * instantiate them.
 *
 * The parent/child hierarchy (ParentComponent) is saved as nesting: an
 * entity's children are listed under its "children" key, so "children" is
 * never a component name. Entities are saved parents first, siblings in id
 * order, so loading into an empty scene recreates them in that order.
 *
 * Layout:
 *   { "formatVersion": 2,
 *     "systems": [ "GrowthSystem", ... ],
 *     "render": { "lighting": {...}, "shadows": {...}, "camera": {...} },
 *     "entities": [ { "TypeName": { "field": value, ... }, ...,
 *                     "children": [ { ... }, ... ] }, ... ] }
 *
 * Format 1 (before hierarchies) is the same without "children".
 */
class SceneSerializer
{
public:
  /**
   * @param types  the reflected types to save/load (usually reflect::types())
   * @param meshes resolves mesh fields to names and back (loading adds a
   *               stand-in for a name it lacks); may be null if no saved
   *               component holds a mesh
   */
  SceneSerializer(const reflect::TypeRegistry& types, MeshLibrary* meshes);

  nlohmann::json save(Scene& scene,
                      const std::vector<std::string>& gameSystems = {}) const;

  /**
   * Add the saved entities to `scene` (existing entities are kept) and apply
   * the saved render settings. The saved game system names go to
   * `gameSystems` when given.
   *
   * @return false (logged) if `json` isn't a scene this version understands.
   */
  bool load(const nlohmann::json& json,
            Scene& scene,
            std::vector<std::string>* gameSystems = nullptr) const;

  /**
   * One object's reflected fields as JSON ({ "field": value, ... }), the form
   * components take in scene files. Works for any reflected type, e.g. a game
   * object script's fields.
   */
  nlohmann::json saveObject(const reflect::TypeInfo& type, void* object) const;

  /**
   * Apply saveObject() output to an object. Fields missing from `fields` keep
   * their current values; values of the wrong type are logged and skipped.
   */
  void loadObject(const reflect::TypeInfo& type, void* object, const nlohmann::json& fields) const;

  /**
   * One entity and everything below it, in the same form as a scene file's
   * entries (its children nested under "children").
   */
  nlohmann::json saveEntity(Scene& scene, const Entity& entity) const;

  /**
   * Create an entity, and the children nested in it, in `scene` from
   * saveEntity() output (e.g. to duplicate a branch), under `parent` (none: a
   * root). Unknown component types are kept, as in load().
   *
   * @return the top entity created
   */
  Entity loadEntity(const nlohmann::json& entry, Scene& scene, const Entity& parent = {}) const;

  /** Save to a file (pretty-printed). @return false (logged) on failure. */
  bool saveFile(Scene& scene,
                const std::filesystem::path& path,
                const std::vector<std::string>& gameSystems = {}) const;

  /** Load from a file. @return false (logged) on failure. */
  bool loadFile(const std::filesystem::path& path,
                Scene& scene,
                std::vector<std::string>* gameSystems = nullptr) const;

  /**
   * Write JSON the way scene files are written: indented, with short arrays
   * on one line, so files stay compact and diff cleanly.
   */
  static bool writeJsonFile(const nlohmann::json& json, const std::filesystem::path& path);

  /** Read a JSON file. @return nothing (logged) if missing or invalid. */
  static std::optional<nlohmann::json> readJsonFile(const std::filesystem::path& path);

private:
  nlohmann::json saveSubtree(const Entity& entity, const hierarchy::EntityTree& tree) const;

  Entity loadEntity(const nlohmann::json& entry,
                    Scene& scene,
                    const Entity& parent,
                    std::set<std::string>* unresolvedTypes) const;

  const reflect::TypeRegistry& m_types;
  MeshLibrary* m_meshes;
};

} // namespace sfs
