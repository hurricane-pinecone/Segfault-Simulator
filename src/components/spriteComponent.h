#pragma once

#include <glm/ext/vector_float3.hpp>
#include <optional>
#include <string>

struct SpriteComponent
{
  std::string assetId;
  int width;
  int height;
  std::optional<glm::vec3> positionInSheet;

  SpriteComponent(const std::string& assetId,
                  int width,
                  int height,
                  std::optional<glm::vec3> positionInSheet = std::nullopt)
      : assetId(assetId), width(width), height(height),
        positionInSheet(positionInSheet)
  {
  }
};
