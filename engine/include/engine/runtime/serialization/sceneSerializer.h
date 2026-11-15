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
#include <unordered_set>
#include <utility>
#include <vector>

namespace sfs
{

class MeshLibrary;
class Scene;

namespace reflect
{
class TypeRegistry;
struct TypeInfo;
struct FieldInfo;
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
 * The parent/child hierarchy (see hierarchy.h) is saved as nesting: an
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
 *
 * A prefab containing itself (through its own content) is left out, logged.
 * What an instance's entry adds belongs to the file holding the entry, so a
 * copy of a prefab added under a copy of itself is no cycle.
 *
 * A scene's plain entry has a "$id" only in a save of a prefab's content open
 * on its own (save() with prefabContent: the editor's prefab tab, its root
 * no longer an instance), so its entities keep their PrefabLinkComponents
 * through a save and load (an undo snapshot) and the prefab isn't renumbered.
 * Loading a plain entry with one gives it back.
 *
 * Entity references (reflect::FieldKind Entity and EntityList fields) are
 * saved as path strings, null for none (a list as an array of them):
 *
 *   "101", "1/2318043377"  a path, as "$overrides" keys are, from the
 *                          context root; "" the context root itself
 *   "../101"               up one path segment first (a nested instance's
 *                          entry in a prefab file naming the outer prefab's)
 *   "/7", "/7/1/2318043377" scene-absolute: the scene entry with
 *                          "$sceneId": 7 (SceneIdComponent), then a path in
 *                          its instance; scene files only
 *   "#3", "#3/1/2318043377" saveEntity() output only, never in a file: the
 *                          entities loading it makes as entries (plain
 *                          entities, instance roots, and what was added in a
 *                          copied instance) in that order, from "#0" (its
 *                          top), then a path in that instance
 *
 * The context root is the prefab's root for a prefab file's own content, and
 * an instance's root for what its entry ("$overrides", "$added") holds. A
 * scene's plain entries have none: theirs are scene-absolute. Saving a
 * reference to an entity a prefab doesn't hold writes null (logged): a prefab
 * can't point outside itself. A scene id ("$sceneId") is given only to an
 * entity a reference names, when a scene or a copy (saveEntity()) is saved:
 * a random 31-bit number not in use in the scene (as Unity's file ids are),
 * so two branches of a scene that each add one don't clash when merged. It's
 * kept from then on. Reading an instance's entry (instanceEntry()) gives none:
 * a reference whose target has none yet is written as null there. References
 * resolve once every entity of the load is
 * made (at the end of an instance, then of the scene or loadEntity() call,
 * or of a LoadSession). One whose target doesn't exist is logged and left
 * empty, its saved value kept in UnresolvedComponents::refs and saved back
 * while the field stays empty.
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
  nlohmann::json savePrefab(const Entity& root,
                            bool keepRootPlacement = false) const;

  /**
   * How an instance differs from its prefab: its scene entry ($prefab,
   * $overrides, $removed, $added, and its $sceneId if it has one), its
   * references saved as a scene's. `root` must have a PrefabInstanceComponent.
   */
  nlohmann::json instanceEntry(const Entity& root) const;

  /**
   * An entity's path in the instance under `root` (see above): "" for the
   * root itself; nothing if it isn't one of the instance's prefab entities
   * (e.g. added to it). `root` is the path's boundary even when it isn't an
   * instance root (a prefab's root while the prefab is edited on its own).
   */
  static std::optional<std::string> pathInInstance(const Entity& entity,
                                                   const Entity& root);

  /**
   * The entity at a path under `root` (the inverse of pathInInstance()).
   *
   * @param root the instance root (or other boundary) the path starts at
   * @param path "$id"s joined by "/" ("" is `root` itself)
   * @return it, or an invalid entity if the path names none
   */
  static Entity entityAtPath(const Entity& root, std::string_view path);

  /**
   * An entity reference read from a file whose target wasn't made yet:
   * resolved once it is (internal to the serializer; a LoadSession holds
   * them).
   */
  struct PendingRef
  {
    /** The entity whose component holds the reference. */
    Entity entity;
    /** The component's type. */
    const reflect::TypeInfo* type = nullptr;
    /** The Entity or EntityList field of it. */
    const reflect::FieldInfo* field = nullptr;
    /** The item of an EntityList field; kNoItem for an Entity field. */
    uint32_t item = 0;
    /** The saved value, a path in the load's expansion keys (see above). */
    std::string value;
    /**
     * The prefab ("asset:<id>") or "Scene" it came from, for messages: a view
     * of the loaded entry's "$prefab", which outlives the load.
     */
    std::string_view source;
  };
  /** PendingRef::item of an Entity (not a list) field. */
  static constexpr uint32_t kNoItem = ~0u;

  /**
   * Scene entries loaded one at a time with loadEntity() (a scene opened over
   * several frames): it holds their scene-absolute references until every
   * entry is in, when finish() resolves them.
   */
  class LoadSession
  {
  private:
    friend class SceneSerializer;
    std::vector<PendingRef> m_refs;
  };

  /**
   * What a field's references kept from a load whose targets didn't exist
   * (UnresolvedComponents::refs), to be saved back while the field (item)
   * stays empty: a string, or for a list an array with null for each item
   * that resolved. Setting null drops it at the next save.
   *
   * @param entity the entity holding the field
   * @param type   the component's type
   * @param field  the Entity or EntityList field
   * @return it, or null if nothing is kept for the field
   */
  static nlohmann::json* keptReferences(const Entity& entity,
                                        const reflect::TypeInfo& type,
                                        const reflect::FieldInfo& field);

  /**
   * The saved value one reference kept (see keptReferences()).
   *
   * @param entity the entity holding the field
   * @param type   the component's type
   * @param field  the Entity or EntityList field
   * @param item   the list's item, or kNoItem for an Entity field
   * @return the value (a string), or null if none is kept
   */
  static const nlohmann::json* keptReference(const Entity& entity,
                                             const reflect::TypeInfo& type,
                                             const reflect::FieldInfo& field,
                                             uint32_t item);

  /** The nearest instance root at or above an entity; invalid if none. */
  static Entity instanceRootOf(const Entity& entity);

  /**
   * The instance whose scene entry holds an entity's components (in its
   * "$overrides" or "$added"), so its references are saved relative to it:
   * the entity's nearest instance root, or the instance that root is part of
   * in turn.
   *
   * @param entity the entity
   * @return that instance's root; invalid if the entity is in none
   */
  static Entity entryRootOf(const Entity& entity);

  /**
   * The scene entry a scene-absolute reference names an entity through (the
   * one whose "$sceneId" it starts with), and the entity's path in it.
   *
   * @param target the entity named
   * @return the outermost instance root it has a path in and that path, or
   *         the entity itself and ""
   */
  static std::pair<Entity, std::string> sceneHolderOf(const Entity& target);

  /**
   * The entities of a scene that have a scene id (SceneIdComponent), by it:
   * what a scene-absolute reference's first segment names. Two with the same
   * id are logged; the first is kept.
   *
   * @param scene the scene
   * @return scene id -> entity
   */
  static std::unordered_map<uint32_t, Entity> sceneIdHolders(Scene& scene);

  /** One entity of an expanded prefab (see expandPrefab()). */
  struct ExpandedEntity
  {
    std::string
        key; // its path in the prefab ("" the root, "3/7" in a nested one)
    std::string parent; // its parent's key (ignored for the root)
    // { "TypeName": { fields... } }, as in scene files, except that entity
    // references are keys of this expansion (relative to its root)
    nlohmann::json components;
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

  /**
   * The scene in scene-file form (see above).
   *
   * @param scene         the scene
   * @param gameSystems   its game systems' names, saved under "systems"
   * @param prefabContent the scene holds a prefab's content, open on its own
   *                      (the editor's prefab tab): plain entries keep their
   *                      prefab "$id"s, so a save and load (an undo snapshot)
   *                      doesn't renumber the prefab's entities
   * @return the JSON
   */
  nlohmann::json save(Scene& scene,
                      const std::vector<std::string>& gameSystems = {},
                      bool prefabContent = false) const;

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
   * components take in scene files, except entity references: with no file
   * to name them in, an Entity field is null (a list, nulls). Works for any
   * reflected type, e.g. a game object script's fields.
   */
  nlohmann::json saveObject(const reflect::TypeInfo& type, void* object) const;

  /**
   * Apply saveObject() output to an object. Fields missing from `fields` keep
   * their current values; values of the wrong type are logged and skipped.
   * Entity fields are left as they are (only a load resolves references).
   */
  void loadObject(const reflect::TypeInfo& type,
                  void* object,
                  const nlohmann::json& fields) const;

  /**
   * One entity and everything below it, in the same form as a scene file's
   * entries (its children nested under "children"), to make a copy with
   * loadEntity(): references inside the branch are "#n", and those outside it
   * scene-absolute, giving their targets scene ids where they have none.
   * Writes no "$sceneId": a copy takes none of the original's.
   */
  nlohmann::json saveEntity(const Entity& entity) const;

  /**
   * Create an entity, and the children nested in it, in `scene` from
   * saveEntity() output (e.g. to duplicate a branch) or a scene file entry,
   * under `parent` (none: a root). Unknown component types are kept, as in
   * load(). Its references resolve at its end, except scene-absolute ones
   * with a `session`, which wait for finish().
   *
   * @return the top entity created
   */
  Entity loadEntity(const nlohmann::json& entry,
                    Scene& scene,
                    const Entity& parent = {},
                    LoadSession* session = nullptr) const;

  /**
   * Resolve the scene-absolute references the loadEntity() calls of a session
   * held back, now that every entry is in. The session is empty after.
   *
   * @param session the session the loadEntity() calls were given
   * @param scene   the scene they loaded into
   */
  void finish(LoadSession& session, Scene& scene) const;

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

  /**
   * One value as writeJsonFile() writes it, for editing a file in place.
   *
   * @param value the value
   * @param depth how deep it sits in the file (the root is 0, a member of the
   *              root 1, ...): what its inner lines are indented by
   * @return its text, without a trailing newline
   */
  static std::string jsonText(const nlohmann::json& value, int depth);

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

  // What one save (a scene, prefab or copy) gathers.
  struct SaveOp
  {
    // Every scene id the scene has or keeps, gathered when a new one is
    // first needed; new ones are drawn until one isn't in it.
    std::unordered_set<uint32_t> usedSceneIds;
    bool scanned = false;
    // How many it has handed out.
    uint32_t given = 0;
  };

  // How the entity references of the entry being written are saved.
  struct RefContext
  {
    // Targets under it with a path are saved as that path, made relative to
    // `fromKey` (its path from here); invalid: none (a scene's plain entry).
    Entity fileRoot;
    std::string fromKey;
    // A target outside fileRoot: scene-absolute (true: a scene, or a
    // saveEntity() branch), or null and logged (false: a prefab file).
    bool sceneAllowed = true;
    // Write each entry's "$sceneId" (a scene file).
    bool sceneIds = false;
    // saveEntity(): the branch's entries by entity id, numbered as loading
    // them makes them ("#n"); null otherwise.
    const std::unordered_map<Entity::EntityId, uint32_t>* branch = nullptr;
    // The save it's part of, which hands out scene ids and drops the kept
    // values of fields set since; null for a read (the public
    // instanceEntry()), which changes nothing.
    SaveOp* op = nullptr;
  };

  // What one load (a scene, an instance, a loadEntity() call) gathers.
  struct LoadOp
  {
    std::set<std::string>* unresolvedTypes = nullptr;
    // References to resolve at its end (scene-absolute, "#n").
    std::vector<PendingRef> refs;
    // When set, gets the entities it makes as entries, in order ("#n").
    std::vector<Entity>* entries = nullptr;
  };

  nlohmann::json
  saveSubtree(const Entity& entity, SavedIds ids, const RefContext& refs) const;
  // An entity's components (reflected and unresolved), without children.
  nlohmann::json saveComponents(const Entity& entity,
                                const RefContext& refs) const;
  // How an instance differs from its prefab, its references saved in `refs`.
  nlohmann::json instanceEntry(const Entity& root,
                               const RefContext& refs) const;
  // The context an instance's entry is saved in, within `outer`'s.
  static RefContext instanceContext(const Entity& root,
                                    const RefContext& outer);
  // The context the components of a scene's entity are saved in.
  static RefContext sceneContext(const Entity& entity, SaveOp& op);
  // The entities added to an instance (not its prefab's), each the top of
  // what's added there, in the order its entry lists them.
  std::vector<Entity> addedTops(const Entity& root) const;
  // One object's fields; an Entity field's value saved in `refs` for
  // `entity` (null without a context).
  nlohmann::json saveFields(const reflect::TypeInfo& type,
                            void* object,
                            const Entity& entity,
                            const RefContext* refs) const;
  // Drop the values UnresolvedComponents::refs keeps for a field's items
  // that are set now (or gone), as a save writes them.
  void forgetSetRefs(const Entity& entity,
                     const reflect::TypeInfo& type,
                     const reflect::FieldInfo& field,
                     void* object) const;
  // One Entity value, or the saved value kept for an empty one.
  nlohmann::json saveRef(const Entity& target,
                         const RefContext& refs,
                         const Entity& entity,
                         const reflect::TypeInfo& type,
                         const reflect::FieldInfo& field,
                         uint32_t item) const;
  // An entity's scene id. One without is given a new one by a save (random,
  // 31 bits, not in use in the scene: two branches of a scene that each add
  // one don't clash when merged), and gets none from a read (no `op`).
  static std::optional<uint32_t> sceneIdFor(const Entity& holder, SaveOp* op);
  // Give every entity a scene's references name scene-absolutely its
  // SceneIdComponent, before any entry is written.
  void giveSceneIds(Scene& scene, SaveOp& op) const;
  // Apply one object's fields; Entity values become `pending`.
  void loadFields(const reflect::TypeInfo& type,
                  void* object,
                  const nlohmann::json& fields,
                  const Entity& entity,
                  std::vector<PendingRef>* pending,
                  std::string_view source) const;
  void loadComponents(const nlohmann::json& components,
                      Entity& entity,
                      std::set<std::string>* unresolvedTypes,
                      std::vector<PendingRef>& pending,
                      std::string_view source) const;
  // Expand an instance entry and create its entities, resolving the
  // references inside it; the rest go to `op`.
  Entity loadInstance(const nlohmann::json& entry,
                      Scene& scene,
                      const Entity& parent,
                      LoadOp& op,
                      std::vector<Entity>* made = nullptr) const;
  // Resolve references, emptying `refs`: paths into an instance through
  // `made` (its entities by expansion key), "#n" through `entries` (a
  // loadEntity() call's), scene-absolute ones through the scene's
  // SceneIdComponents. With `defer`, what `made` doesn't cover (with no
  // `made`, the scene-absolute ones) moves there instead.
  void resolveRefs(std::vector<PendingRef>& refs,
                   Scene& scene,
                   const std::vector<Entity>* entries,
                   const std::unordered_map<std::string, Entity>* made,
                   std::vector<PendingRef>* defer) const;
  // What an entry alone gives (for an instance entry, its prefab and its own
  // overrides): path -> (parent path, components), with every known
  // component's fields filled in, for comparing an instance against.
  std::vector<PrefabNode> expandBase(const nlohmann::json& entry) const;

  Entity loadEntity(const nlohmann::json& entry,
                    Scene& scene,
                    const Entity& parent,
                    LoadOp& op) const;

  const reflect::TypeRegistry& m_types;
  MeshLibrary* m_meshes;
  PrefabSource m_prefabs;
  // Default field values by component type, for normalising prefab data.
  mutable std::unordered_map<std::string, nlohmann::json> m_defaults;
};

} // namespace sfs
