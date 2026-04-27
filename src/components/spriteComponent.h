#pragma once

#include <string>

struct SpriteComponent
{
  std::string assetId;
  int width;
  int height;

  SpriteComponent(std::string assetId, int width, int height)
      : assetId(std::move(assetId)), width(width), height(height)
  {
  }
};
