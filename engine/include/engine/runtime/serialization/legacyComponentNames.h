#pragma once

#include "nlohmann/json.hpp"

namespace sfs
{

/**
 * Rename components saved under a name the engine has since retired (e.g.
 * "Transform3DComponent", from before flat, isometric and 3D shared one
 * transform) to their current names, anywhere in a scene or prefab file read
 * from disk, and fold retired ones into the component that took them over
 * ("SpriteTint" and "ShadowCasterComponent" into SpriteComponent's tint and
 * castsShadow). The file is rewritten the new way the next time it's saved.
 */
void upgradeComponentNames(nlohmann::json& file);

} // namespace sfs
