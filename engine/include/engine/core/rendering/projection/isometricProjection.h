#pragma once

#include "engine/core/rendering/iProjection.h"
#include "engine/core/rendering/util/isometric/geometry.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

struct IsometricProjectionConfig
{
  int tileWidth = 0;
  int tileHeight = 0;
  int elevationStep = 8;

  float worldScale = 1.0f;

  glm::vec2 screenCenter{0.0f, 0.0f};
};

/**
 * Isometric heightfield projection: maps a grid position plus an integer
 * elevation level to screen pixels (and back). `elevation` is in elevation
 * levels, not pixels. Overrides are `final` so calls through the concrete type
 * devirtualize on the per-sprite/per-vertex hot path.
 */
struct IsometricProjection : public IProjection
{
  int tileWidth = 0;
  int tileHeight = 0;
  int elevationStep = 8;

  float worldScale = 1.0f;
  float zoom = 1.0f;

  /// Screen-space iso offset of the camera, i.e.
  /// gridToIsometric(cameraGridPosition, tileWidth, tileHeight, worldScale).
  glm::vec2 cameraIso{0.0f, 0.0f};

  /// Pixel position that grid origin maps to before the camera offset is
  /// applied (typically the window centre).
  glm::vec2 screenCenter{0.0f, 0.0f};

  glm::vec2 worldToScreen(const glm::vec2& world, float elevation) const final;
  glm::vec2 screenToWorld(const glm::vec2& screen, float elevation) const final;
  float worldUnitToPixels() const final;
};

/**
 * Pick the topmost terrain tile under a screen point, walking elevation planes
 * from the highest down so a block's vertical wall resolves to its own tile.
 * Falls back to the flat ground projection (with `valid == false`) when no tile
 * lies under the cursor.
 */
TilePick pickTile(const glm::vec2& screen,
                  const IsometricProjection& projection,
                  const TerrainElevationGridView& terrain);

} // namespace sfs
