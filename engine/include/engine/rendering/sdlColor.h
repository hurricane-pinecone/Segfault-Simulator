#pragma once

#include "engine/Color/Color.h"
#include <SDL_pixels.h>

namespace sfs
{

// SDL_Color view of an sfs::Color, for the SDL-backed render/text paths. Lives on
// the render side so engine/Color stays free of SDL.
constexpr SDL_Color toSDL(const Color& c)
{
  return SDL_Color{c.r, c.g, c.b, c.a};
}

} // namespace sfs
