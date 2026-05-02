#pragma once

#include <SDL2/SDL_render.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

struct SpriteSheetPosition
{
  int x, y, gap;
};

struct Sprite
{
  uint32_t id;
  std::string textureId;
  std::string name;
  uint16_t width;
  uint16_t height;

  std::optional<SpriteSheetPosition> positionInSheet;
};
