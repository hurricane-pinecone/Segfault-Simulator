#pragma once

#include "engine/core/ecs/entity.h"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
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
} // namespace reflect

/**
 * Saves and loads a scene as JSON, driven by reflection: every reflected
 * component on every entity is written field by field, and read back by type
 * and field name. A component type or field the registry doesn't know (renamed,
 * removed, or from a module that isn't loaded) is reported and skipped, so a
 * scene still loads after its code changes. Meshes are stored by their
 * MeshLibrary name.
 *
 * When the scene has a RenderSystem, its lighting, shadow and camera settings
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
 *   { "formatVersion": 3,
 *     "systems": [ "GrowthSystem", ... ],
 *     "render": { "style": { "kind": "3d" | "flat" | "isometric", ... },
 *                 "lighting": {...}, "shadows": {...}, "camera": {...} },
 *     "entities": [ { "TypeName": { "field": value, ... }, ...,
 *                     "children": [ { ... }, ... ] }, ... ] }
 *
 * Format 1 (before hierarchies) is the same without "children". Format 2 is
 * format 3 with model nodes' old ids (see below).
 *
 * Prefabs: a .prefab file is { "formatVersion": 2, "root": <entry> }, entries
 * as in scenes plus a "$id" on each (unique in the file; the root's isn't
 * used). A model's prefab (made from the model file, see AssetStore) also has
 * "model": true, which gives each entity it makes a RestPoseComponent: its
 * transform as loaded, after the instance's overrides. Its nodes' "$id"s are
 * their stable ids (ModelNode::id, made from their name paths, with bit 31
 * set), so reordering the model file's nodes moves nothing. An instance of a
 * prefab, in a scene or a prefab, is saved as the prefab and how it differs
 * from it:
 *
 *   { "$prefab": "asset:<id>",
 *     "$overrides": { "<path>": { "Component": { "field": value } }, ... },
 *     "$removed": [ "<path>", ... ],
 *     "$added": [ { "$parent": "<path>", <entry> }, ... ] }
 *
 * A path names an entity of the instance by its prefab "$id" ("" is the
 * instance's root), through nested instances ("3/7": entity 7 of the instance
 * that is entity 3). Overrides are JSON merge patches of the entity's
 * components (null removes one). They're found on save by comparing each
 * entity with what the prefab alone gives, so any change is kept.
 *
 * Older files (prefab format 1, scene format 2) named a model's nodes by
 * their place in the model file's node order (1, 2, ...). Such a path segment
 * into a model is told apart by its value (stable ids have bit 31 set) and
 * read as the node in that place, so older files load onto the same nodes;
 * nodeIdEdits() gives the edits that upgrade a file for good. One past the
 * model's nodes is read and saved as "old:<id>", which names nothing.
 *
 * An entity a prefab adds inside one of its nested instances (an "$added" of
 * a nested instance's entry) belongs to that prefab: it has a "$id" with
 * kAddedNodeIdBit set (made from its place in the entry when the file has
 * none) and is named, like the nested instance's own entities, by a path
 * through it ("3/1073741907"), so instances of the prefab know it as theirs.
 *
 * An override, removed path or added entity's "$parent" naming an entity the
 * prefab no longer has (a model node renamed or deleted in the source file)
 * is kept, unused, in the instance root's UnresolvedComponents::overrides,
 * logged once at load, and saved back unchanged.
 */
class SceneSerializer
{
public:
  /** The scene file format this engine writes (and the newest it reads). */
  static constexpr int kSceneFormatVersion = 3;
  /** The prefab file format this engine writes. */
  static constexpr int kPrefabFormatVersion = 2;

  /**
   * @param types  the reflected types to save/load (usually reflect::types())
   * @param meshes resolves mesh fields to names and back (loading adds a
   *               stand-in for a name it lacks); may be null if no saved
   *               component holds a mesh
   */
  SceneSerializer(const reflect::TypeRegistry& types, MeshLibrary* meshes);

  /** A prefab's name ("asset:<id>") -> its file's contents; nothing if none. */
  using PrefabSource =
      std::function<std::optional<nlohmann::json>(std::string_view name)>;

  /**
   * Where prefabs come from. Without one, instances can't be expanded: they
   * load as placeholders that save back unchanged.
   */
  void setPrefabSource(PrefabSource source) { m_prefabs = std::move(source); }

  /**
   * Create an instance of a prefab in `scene`, under `parent` (none: a root).
   * `made`, when given, gets every entity created, parents first.
   *
   * @return its root, or a placeholder (logged) if the prefab can't be read
   */
  Entity instantiate(std::string_view prefab,
                     Scene& scene,
                     const Entity& parent = {},
                     std::vector<Entity>* made = nullptr) const;

  /**
   * A prefab file's contents ({ "formatVersion", "root" }) made from an entity
   * and everything below it: entities that came from a prefab keep their ids,
   * the rest are given new ones (recorded on them, so they belong to the
   * prefab from now on). Instances below the root are saved as instances. For
   * an instance root (Apply), its prefab's root transform is kept rather than
   * the instance's; otherwise the root is moved to the origin, unless
   * `keepRootPlacement` (saving a prefab edited on its own).
   */
  nlohmann::json savePrefab(Scene& scene,
                            const Entity& root,
                            bool keepRootPlacement = false) const;

  /**
   * How an instance differs from its prefab: its scene entry ($prefab,
   * $overrides, $removed, $added). `root` must have a PrefabInstanceComponent.
   */
  nlohmann::json instanceEntry(Scene& scene, const Entity& root) const;

  /**
   * An entity's path in the instance under `root` (see above): "" for the
   * root itself; nothing if it isn't one of the instance's prefab entities
   * (e.g. added to it).
   */
  static std::optional<std::string> pathInInstance(const Entity& entity,
                                                   const Entity& root);

  /** The nearest instance root at or above an entity; invalid if none. */
  static Entity instanceRootOf(const Entity& entity);

  /** One entity of an expanded prefab (see expandPrefab()). */
  struct ExpandedEntity
  {
    std::string
        key; // its path in the prefab ("" the root, "3/7" in a nested one)
    std::string parent; // its parent's key (ignored for the root)
    nlohmann::json
        components; // { "TypeName": { fields... } }, as in scene files
  };

  /**
   * A prefab's entities as they'd be instantiated (nested prefabs expanded,
   * their overrides applied), parents first, without making them: e.g. to
   * draw a preview. Empty if the prefab can't be read.
   */
  std::vector<ExpandedEntity> expandPrefab(std::string_view prefab) const;

  /**
   * The entities a scene or prefab file entry makes (as expandPrefab(), for
   * an instance entry with its own overrides, or a plain entry and the
   * instances under it), parents first, without making them.
   *
   * @param entry an entry of a scene's "entities", or a prefab's "root"
   */
  std::vector<ExpandedEntity> expandEntry(const nlohmann::json& entry) const;

  /**
   * One string in a prefab or scene file that names a model node by its old
   * id (its place in the model file's node order), and what it becomes with
   * the node's stable id (see nodeIdEdits()).
   */
  struct NodeIdEdit
  {
    /** The JSON pointer of the string in the file. */
    std::string pointer;
    /** It's an object key (an "$overrides" path), not a string value. */
    bool key = false;
    /** The path as the file has it. */
    std::string from;
    /** The path with the stable id. */
    std::string to;
    /** The model ("asset:<id>") whose node the path names. */
    std::string model;
    /** The node's old id (its place in the model's node order, from 1). */
    uint32_t fromId = 0;
    /** The node's stable id (ModelNode::id); 0 if the model has none there. */
    uint32_t toId = 0;
  };

  /**
   * The edits that upgrade a prefab or scene file to stable model node ids:
   * each "$overrides" key, "$removed" path and "$added" "$parent" that names
   * a model node by its old id, in any instance entry of the file (nested
   * ones included). An old id past the model's nodes becomes "old:<id>", so
   * it stays unused (and logged) and never names a node again. Empty for a
   * file that's up to date.
   *
   * @param file       a parsed prefab ({ "root": ... }) or scene
   *                   ({ "entities": ... }) file
   * @param unreadable when given, gets the name of each prefab an instance in
   *                   the file names that can't be read (its paths can't be
   *                   upgraded)
   * @return the edits, in the order they appear in the file's instances
   */
  std::vector<NodeIdEdit>
  nodeIdEdits(const nlohmann::json& file,
              std::vector<std::string>* unreadable = nullptr) const;

  // One entity of an expanded instance (internal to the serializer).
  struct PrefabNode;

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
  void loadObject(const reflect::TypeInfo& type,
                  void* object,
                  const nlohmann::json& fields) const;

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
  Entity loadEntity(const nlohmann::json& entry,
                    Scene& scene,
                    const Entity& parent = {}) const;

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
  static bool writeJsonFile(const nlohmann::json& json,
                            const std::filesystem::path& path);

  /** Read a JSON file. @return nothing (logged) if missing or invalid. */
  static std::optional<nlohmann::json>
  readJsonFile(const std::filesystem::path& path);

private:
  // Which entities a saved branch writes the prefab "$id" of: none, all (a
  // prefab file's own content), or only those a prefab added inside a nested
  // instance (kAddedNodeIdBit), saved in that instance's "$added".
  enum class SavedIds
  {
    None,
    All,
    Added,
  };
  nlohmann::json saveSubtree(const Entity& entity,
                             const hierarchy::EntityTree& tree,
                             SavedIds ids = SavedIds::None) const;
  // An entity's components (reflected and unresolved), without children.
  nlohmann::json saveComponents(const Entity& entity) const;
  void loadComponents(const nlohmann::json& components,
                      Entity& entity,
                      std::set<std::string>* unresolvedTypes) const;
  // Expand an instance entry and create its entities.
  Entity loadInstance(const nlohmann::json& entry,
                      Scene& scene,
                      const Entity& parent,
                      std::set<std::string>* unresolvedTypes,
                      std::vector<Entity>* made = nullptr) const;
  // What an entry alone gives (for an instance entry, its prefab and its own
  // overrides): path -> (parent path, components), with every known
  // component's fields filled in, for comparing an instance against.
  std::vector<PrefabNode> expandBase(const nlohmann::json& entry) const;
  nlohmann::json instanceEntry(const Entity& root,
                               const hierarchy::EntityTree& tree) const;

  Entity loadEntity(const nlohmann::json& entry,
                    Scene& scene,
                    const Entity& parent,
                    std::set<std::string>* unresolvedTypes) const;

  const reflect::TypeRegistry& m_types;
  MeshLibrary* m_meshes;
  PrefabSource m_prefabs;
  // Default field values by component type, for normalising prefab data.
  mutable std::unordered_map<std::string, nlohmann::json> m_defaults;
};

} // namespace sfs
