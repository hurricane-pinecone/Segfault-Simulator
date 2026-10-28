#pragma once

#include "nlohmann/json.hpp"

namespace sfs
{

/**
 * Saved component data whose type isn't registered right now -- typically a
 * game component whose module hasn't been built or loaded yet. SceneSerializer
 * keeps it here, verbatim, instead of dropping it: it is written back unchanged
 * on save, and becomes a real component the next time the scene loads with the
 * type registered.
 *
 * @param nlohmann::json components - { "TypeName": { fields... }, ... }
 */
struct UnresolvedComponents
{
  nlohmann::json components = nlohmann::json::object();
};

} // namespace sfs
