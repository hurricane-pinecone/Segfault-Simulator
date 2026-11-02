#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sfs
{

class AssetDatabase;
class RenderSystem;

/**
 * A project's visual styles (.style assets): (name, path) each, the name
 * ("asset:<id>") as RenderSystem::setVisualStyle takes it, sorted by path.
 */
std::vector<std::pair<std::string, std::string>>
visualStyleAssets(const AssetDatabase& assets);

/**
 * Switch `render` to the project's next visual style (`step` -1: the one
 * before), wrapping around; from none (or one that isn't the project's), the
 * first (or last). What the development key (F8, see Game::setDevKeys) does.
 *
 * @return the path of the style now active; empty if the project has none
 */
std::string cycleVisualStyle(RenderSystem& render,
                             const AssetDatabase& assets,
                             int step = 1);

} // namespace sfs
