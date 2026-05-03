#pragma once

#include <SDL2/SDL_render.h>
#include <SDL_rect.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sfs
{

struct SpriteRegion
{
  std::string name;
  SDL_Rect srcRect;
};

struct Sprite
{
  uint32_t id;
  std::string textureId;
  std::string name;

  SDL_Rect srcRect;
};

} // namespace sfs
