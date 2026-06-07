#pragma once

#include "engine/core/Color/Color.h"

namespace sfs
{

/**
 * Marks a tile as water at the given elevation, drawn as an animated surface
 * tinted by color. Read by the isometric water pass.
 *
 * @param int elevation - water surface elevation level, default 0
 * @param Color color - RGBA water tint, default (35, 120, 190, 120)
 */
struct WaterTileComponent
{
  int elevation = 0;
  Color color{35, 120, 190, 120};
};

} // namespace sfs
