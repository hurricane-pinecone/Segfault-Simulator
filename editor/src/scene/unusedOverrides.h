#pragma once

#include "engine/core/ecs/entity.h"
#include "nlohmann/json.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{
struct ModelNode;
} // namespace sfs

// The parts of an instance's saved entry that name entities its prefab no
// longer has (a model node renamed or deleted in its source file): kept,
// unused, on the instance root (UnresolvedComponents::overrides) until they're
// reattached to entities it has now, or removed.
namespace unusedOverrides
{

/** One unused part of an instance's saved entry. */
struct Unused
{
  enum class Kind
  {
    Override, // an "$overrides" entry
    Removed,  // a "$removed" path
    Added,    // an "$added" entity, by its "$parent"
  };
  /** Which part of the entry it is. */
  Kind kind = Kind::Override;
  /**
   * The path it names, relative to the instance: the override's key, the
   * removed path, or the added entity's parent.
   */
  std::string path;
  /** The override's patch, or the added entity's entry; null for a removal. */
  nlohmann::json value;
};

/**
 * The unused parts of an instance's saved entry.
 *
 * @param root the instance's root
 * @return them, overrides first, then removals, then added entities; none if
 *         it has none
 */
std::vector<Unused> of(const sfs::Entity& root);

/**
 * Move paths of an instance's entry: each "$overrides" key, "$removed" path
 * and "$added" "$parent" in `moves` becomes its new path. An override moved
 * onto one the entry already has is merged into it, its values winning.
 *
 * @param entry the instance's entry (SceneSerializer::instanceEntry())
 * @param moves old path -> new path, relative to the instance
 */
void move(nlohmann::json& entry,
          const std::map<std::string, std::string>& moves);

/**
 * Drop one part from an instance's entry.
 *
 * @param entry the instance's entry (SceneSerializer::instanceEntry())
 * @param item  what to drop, as of() gave it
 */
void remove(nlohmann::json& entry, const Unused& item);

/**
 * The moves that take a model node renamed in its source file, and every node
 * below it, from their old ids to their ids now: each old id is worked out
 * from the node's name path now with its old name put back (see
 * sfs::stableNodeId()). Only paths an unused part of the instance's entry
 * names are moved.
 *
 * @param root    the instance whose entry has the unused parts
 * @param node    the renamed node, as the instance has it now
 * @param oldName its name before
 * @param nodes   the model's nodes (sfs::importModelNodes())
 * @param paths   their name paths, in the same order
 * @return old path -> new path, relative to `root`
 */
std::map<std::string, std::string>
renamedNodeMoves(const sfs::Entity& root,
                 const sfs::Entity& node,
                 std::string_view oldName,
                 const std::vector<sfs::ModelNode>& nodes,
                 const std::vector<std::string>& paths);

} // namespace unusedOverrides
