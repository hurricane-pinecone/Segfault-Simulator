#pragma once

#include "engine/rendering/util/isometric/camera.h"
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
  // Non-null while the render pipeline runs; the forwarders below assume it.
  const IsometricProjection* projection = nullptr;

  const IsometricAmbientLighting* ambientLighting = nullptr;
  const std::vector<IsometricPointLightSnapshot>* pointLights = nullptr;

  TerrainElevationGridView terrainElevationGrid;

  // True while terrain renders as real block-face geometry (the BlockGeometry
  // module is registered) rather than billboard sprites. Modules that differ
  // between the two render styles (e.g. projected terrain shadows) branch on it.
  bool geometryActive = false;

  glm::ivec2 gridCellOf(const glm::vec2& position) const
  {
    return sfs::gridCellOf(position);
  }

  glm::vec2 worldToScreen(const glm::vec2& world, float elevation) const
  {
    return projection->worldToScreen(world, elevation);
  }

  glm::vec2 screenToWorld(const glm::vec2& screen, float elevation = 0.0f) const
  {
    return projection->screenToWorld(screen, elevation);
  }

  TilePick pickTile(const glm::vec2& screen) const
  {
    return sfs::pickTile(screen, *projection, terrainElevationGrid);
  }
};
} // namespace sfs
