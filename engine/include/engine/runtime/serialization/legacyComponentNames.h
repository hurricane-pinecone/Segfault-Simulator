#pragma once

#include "nlohmann/json.hpp"

namespace sfs
{

/**
 * Rename components saved under a name the engine has since retired (e.g.
 * "Transform3DComponent", from before flat, isometric and 3D shared one
 * transform) to their current names, anywhere in a scene or prefab file read
 * from disk. The file is rewritten with the new names the next time it's
 * saved.
 */
void upgradeComponentNames(nlohmann::json& file);

} // namespace sfs
