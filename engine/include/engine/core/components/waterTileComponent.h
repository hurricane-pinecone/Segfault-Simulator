#pragma once

#include "engine/core/Color/Color.h"

namespace sfs
{

struct WaterTileComponent
{
  int elevation = 0;
  Color color{35, 120, 190, 120};
};

} // namespace sfs
