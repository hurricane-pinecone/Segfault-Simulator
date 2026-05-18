
#pragma once

#include "SDL2/SDL_rect.h"
#include "engine/systems/isometric/isometricLightingSystem.h"
#include <string>

namespace sfs
{

struct IsometricRenderItem
{
  const std::string* textureId = nullptr;

  SDL_Rect srcRect{0, 0, 0, 0};
  SDL_Rect dest{0, 0, 0, 0};

  int textureWidth = 0;
  int textureHeight = 0;

  float sortKey = 0.0f;
  int renderLayer = 1;
  float screenSortY = 0.0f;

  bool hasNormalMap = false;
  const std::string* normalTextureId = nullptr;
  SDL_Rect normalSrcRect{0, 0, 0, 0};
  int normalTextureWidth = 0;
  int normalTextureHeight = 0;

  IsometricComputedLighting lighting{};

  bool isShadow = false;
  bool isTerrainShadow = false;
  glm::vec2 shadowOffset{0.0f, 0.0f};
  glm::vec2 shadowScreenPoints[4] = {};

  SDL_Color tint{255, 255, 255, 255};

  glm::vec2 worldPoints[4] = {
      glm::vec2{0.0f, 0.0f},
      glm::vec2{0.0f, 0.0f},
      glm::vec2{0.0f, 0.0f},
      glm::vec2{0.0f, 0.0f},
  };
};

} // namespace sfs
