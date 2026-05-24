#pragma once

#include "engine/rendering/util/isometric/geometry.h"
#include "engine/utils/isometricLightingUtils.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace sfs
{

struct IVec2Hash
{
  std::size_t operator()(const glm::ivec2& v) const noexcept
  {
    const std::uint32_t x = static_cast<std::uint32_t>(v.x);
    const std::uint32_t y = static_cast<std::uint32_t>(v.y);

    return static_cast<std::size_t>(x * 73856093u ^ y * 19349663u);
  }
};

struct IsometricRenderContext
{
  int windowWidth = 0;
  int windowHeight = 0;

  int tileWidth = 0;
  int tileHeight = 0;
  int elevationStep = 8;

  float worldScale = 1.0f;

  float zoom = 1.0f;
  glm::vec2 screenCenter{0.0f, 0.0f};
  glm::vec2 isoCameraPosition{0.0f, 0.0f};

  bool waveEnabled = false;
  float waveTime = 0.0f;
  float waveAmplitude = 6.0f;
  float waveFrequency = 0.45f;
  float waveSpeed = 3.0f;

  const IsometricAmbientLighting* ambientLighting = nullptr;
  const std::vector<IsometricPointLightSnapshot>* pointLights = nullptr;

  TerrainElevationGridView terrainElevationGrid;

  const std::unordered_map<glm::ivec2, int, IVec2Hash>* tileElevations =
      nullptr;

  glm::vec2 gridToIsometric(const glm::vec2& gridPosition) const
  {
    return {
        (gridPosition.x - gridPosition.y) *
            (static_cast<float>(tileWidth) * worldScale) / 2.0f,

        (gridPosition.x + gridPosition.y) *
            (static_cast<float>(tileHeight) * worldScale) / 2.0f,
    };
  }

  glm::ivec2 gridCellOf(const glm::vec2& position) const
  {
    return {
        static_cast<int>(std::floor(position.x)),
        static_cast<int>(std::floor(position.y)),
    };
  }

  float getWaveOffset(const glm::vec2& gridPosition) const
  {
    if (!waveEnabled)
      return 0.0f;

    return std::sin(gridPosition.x * waveFrequency +
                    gridPosition.y * waveFrequency + waveTime * waveSpeed) *
           waveAmplitude;
  }

  glm::vec2 worldToScreen(glm::vec2 p, float elevation) const
  {
    glm::vec2 screen =
        (gridToIsometric(p) - isoCameraPosition) * zoom + screenCenter;

    screen.y -= elevation * elevationStep * worldScale * zoom;
    screen.y -= getWaveOffset(p) * zoom;

    return screen;
  }

  bool hasTileAt(const glm::ivec2& tile) const
  {
    return tileElevations &&
           tileElevations->find(tile) != tileElevations->end();
  }

  int getTileElevationAt(const glm::vec2& position) const
  {
    if (!tileElevations)
      return 0;

    const glm::ivec2 tile = gridCellOf(position);

    auto it = tileElevations->find(tile);

    if (it == tileElevations->end())
      return 0;

    return it->second;
  }
};
} // namespace sfs
