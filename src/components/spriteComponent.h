#pragma once

#include <glm/ext/vector_float2.hpp>
#include <optional>
#include <string>

struct SpriteComponent
{
  std::string assetId;
  int width;
  int height;
  std::optional<glm::vec2> positionInSheet;

  SpriteComponent(std::string assetId,
                  int width,
                  int height,
                  std::optional<glm::vec2> positionInSheet = std::nullopt)
      : assetId(std::move(assetId)), width(width), height(height),
        positionInSheet(positionInSheet)
  {
  }
};
