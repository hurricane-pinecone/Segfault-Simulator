#pragma once

#include "SDL2/SDL_pixels.h"

namespace sfs
{

struct WaterTileComponent
{
  int elevation = 0;
  SDL_Color color{35, 120, 190, 120};
};

} // namespace sfs
