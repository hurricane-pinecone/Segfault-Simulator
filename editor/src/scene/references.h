#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/runtime/serialization/sceneSerializer.h"
#include "nlohmann/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sfs
{
class Scene;
namespace reflect
{
struct TypeInfo;
struct FieldInfo;
} // namespace reflect
} // namespace sfs

// The editor's side of entity reference fields (reflect::FieldKind Entity and
// EntityList): where an entity's references are named from, the saved value a
// reference keeps while its target is missing, copying a reference from one
// selected object to another, and keeping references into an instance that is
// rebuilt.
namespace references
{

/** The item of an Entity field (not a list). */
inline constexpr uint32_t kNoItem = sfs::SceneSerializer::kNoItem;

/**
 * The entity an entity's references are named from, as its file names them:
 * the instance whose entry holds it (SceneSerializer::entryRootOf()), or, in a
 * prefab's content open on its own, the prefab's root. A scene's plain entity
 * has none: its references are scene-absolute.
 *
 * @param entity        the entity holding the references
 * @param prefabContent the scene is a prefab's content (a prefab's tab)
 * @return that root; invalid if there's none
 */
sfs::Entity contextRoot(const sfs::Entity& entity, bool prefabContent);

/**
 * The saved value a reference kept when it loaded without its target
 * (SceneSerializer::keptReference()), shown until the field is set or
 * cleared.
 *
 * @param entity the entity holding the field
 * @param type   the component's type
 * @param field  the Entity or EntityList field
 * @param item   the list's item, or kNoItem
 * @return the value ("1/2318043377"), or null if none is kept
 */
const std::string* keptValue(const sfs::Entity& entity,
                             const sfs::reflect::TypeInfo& type,
                             const sfs::reflect::FieldInfo& field,
                             uint32_t item);

/**
 * Stop keeping a reference's saved value (the field cleared): the next save
 * writes the field (item) as none.
 *
 * @param entity the entity holding the field
 * @param type   the component's type
 * @param field  the Entity or EntityList field
 * @param item   the list's item, or kNoItem
 */
void forgetKept(const sfs::Entity& entity,
                const sfs::reflect::TypeInfo& type,
                const sfs::reflect::FieldInfo& field,
                uint32_t item);

/**
 * A list's item was taken out: the values kept for the items after it move
 * up with them.
 *
 * @param entity the entity holding the field
 * @param type   the component's type
 * @param field  the EntityList field
 * @param item   the item taken out
 */
void eraseKeptItem(const sfs::Entity& entity,
                   const sfs::reflect::TypeInfo& type,
                   const sfs::reflect::FieldInfo& field,
                   std::size_t item);

/**
 * A reference field's value as several selected objects are compared by: each
 * target as its path from the holder's context root (contextRoot()) when it's
 * under it with a path, so objects that each name their own part compare
 * equal, else as the target's id; an empty one as {"missing": value} when it
 * keeps a saved value (SceneSerializer::keptReference()), else null. A list
 * gives an array of them.
 *
 * @param holder        the entity holding the field
 * @param type          the component's type
 * @param field         the Entity or EntityList field
 * @param object        the component
 * @param prefabContent the scene is a prefab's content (a prefab's tab)
 * @return the value to compare
 */
nlohmann::json comparable(const sfs::Entity& holder,
                          const sfs::reflect::TypeInfo& type,
                          const sfs::reflect::FieldInfo& field,
                          void* object,
                          bool prefabContent);

/**
 * What one reference becomes copied from one object to another: a target
 * under the source's context root is named by its path there and found under
 * the other object's own root, so each gets its own part; any other target is
 * copied as it is.
 *
 * @param target        the reference's target on the source object
 * @param from          the entity holding the source
 * @param to            the entity it's copied to
 * @param prefabContent the scene is a prefab's content (a prefab's tab)
 * @return the target for `to`, or nothing (logged) if its root has no entity
 *         at that path
 */
std::optional<sfs::Entity> copied(const sfs::Entity& target,
                                  const sfs::Entity& from,
                                  const sfs::Entity& to,
                                  bool prefabContent);

/**
 * Copy a reference field from one object to another (several selected and
 * edited together), each target as copied() gives it, and the saved values
 * the source keeps for its empty items with it: a cleared reference clears
 * the other's kept value too, and a list's stay with their items.
 *
 * @param type          the component's type
 * @param field         the Entity or EntityList field
 * @param from          the entity holding the source
 * @param fromObject    its component
 * @param to            the entity it's copied to
 * @param toObject      its component
 * @param prefabContent the scene is a prefab's content (a prefab's tab)
 * @return false (the field left as it was) if a target can't be named for
 *         `to`
 */
bool copy(const sfs::reflect::TypeInfo& type,
          const sfs::reflect::FieldInfo& field,
          const sfs::Entity& from,
          void* fromObject,
          const sfs::Entity& to,
          void* toObject,
          bool prefabContent);

/**
 * A reference held outside an instance to an entity in it, to be found again
 * once the instance is rebuilt (its entities made anew).
 */
struct Inbound
{
  /** The entity holding the reference (outside the instance). */
  sfs::Entity holder;
  /** The component's type. */
  const sfs::reflect::TypeInfo* type = nullptr;
  /** The Entity or EntityList field. */
  const sfs::reflect::FieldInfo* field = nullptr;
  /** The list's item, or kNoItem. */
  uint32_t item = kNoItem;
  /**
   * The scene id of the entry the target is named through
   * (SceneSerializer::sceneHolderOf()); nothing if it has none, when `path`
   * is from the instance's root.
   */
  std::optional<uint32_t> sceneId;
  /** The target's path from that entry, or from the instance's root. */
  std::string path;
};

/**
 * The references other entities hold to an instance's entities, before it's
 * rebuilt (see restore()).
 *
 * @param root the instance's root
 * @return each such reference, with how to find its target again
 */
std::vector<Inbound> inboundTo(const sfs::Entity& root);

/**
 * Point the references inboundTo() found at the rebuilt instance's entities.
 * One whose target the rebuilt instance doesn't have is left empty (logged).
 *
 * @param refs  what inboundTo() gave before the rebuild
 * @param root  the rebuilt instance's root
 * @param scene the scene it's in
 */
void restore(const std::vector<Inbound>& refs,
             const sfs::Entity& root,
             sfs::Scene& scene);

/**
 * An entity's place under another, as names: "Body/Arm/Hand" (entityLabel()s
 * from below `root` down to it). Ids are never shown in a reference slot.
 *
 * @param entity the entity
 * @param root   where the path starts (not part of it); invalid: the top
 * @return the names joined with "/"
 */
std::string namePath(const sfs::Entity& entity, const sfs::Entity& root);

} // namespace references
