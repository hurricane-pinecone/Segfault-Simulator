#pragma once

#include "nlohmann/json.hpp"

namespace sfs
{

/**
 * Saved data that couldn't be applied when its entity loaded, kept to be
 * written back. SceneSerializer keeps it here, verbatim, instead of dropping
 * it: it is saved back unchanged, and applies the next time the entity loads
 * once it can.
 */
struct UnresolvedComponents
{
  /**
   * Components whose type isn't registered right now (typically a game
   * component whose module hasn't been built or loaded yet):
   * { "TypeName": { fields... }, ... }.
   */
  nlohmann::json components = nlohmann::json::object();
  /**
   * On an instance root: the parts of its saved entry that name entities its
   * prefab no longer has (a model node renamed or deleted in its source
   * file), in the entry's own form: { "$overrides": { path: patch },
   * "$removed": [ path ], "$added": [ entry ] }, each only if it has any,
   * with "$removedAt" and "$addedAt" giving each list item's place in the
   * entry, so it's saved back there. Null when there are none.
   */
  nlohmann::json overrides;
};

} // namespace sfs
