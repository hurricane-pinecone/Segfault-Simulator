#include "engine/core/rendering/projection/isometricProjection.h"

#include "glm/glm/ext/vector_int2.hpp"

namespace sfs
{

glm::vec2 IsometricProjection::worldToScreen(const glm::vec2& world,
                                             float elevation) const
{
  glm::vec2 p = (gridToIsometric(world, tileWidth, tileHeight, worldScale) -
                 cameraIso) *
                    zoom +
                screenCenter;

  p.y -= elevation * static_cast<float>(elevationStep) * worldScale * zoom;

  return p;
}

glm::vec2 IsometricProjection::screenToWorld(const glm::vec2& screen,
                                             float elevation) const
{
  glm::vec2 p = screen;

  p.y += elevation * static_cast<float>(elevationStep) * worldScale * zoom;

  const glm::vec2 iso = (p - screenCenter) / zoom + cameraIso;

  return isometricTogrid(iso, tileWidth, tileHeight, worldScale);
}

float IsometricProjection::worldUnitToPixels() const
{
  return static_cast<float>(tileWidth) * worldScale * zoom;
}

TilePick pickTile(const glm::vec2& screen,
                  const IsometricProjection& projection,
                  const TerrainElevationGridView& terrain)
{
  const int maxElevation = maxTerrainElevation(terrain);

  for (int elevation = maxElevation; elevation >= 0; elevation--)
  {
    const glm::vec2 world =
        projection.screenToWorld(screen, static_cast<float>(elevation));

    const glm::ivec2 tile = gridCellOf(world);

    int tileElevation = 0;

    // Take the first (topmost) cell whose column reaches the tested plane.
    // The >= also resolves a block's vertical walls, where the cursor projects
    // onto the tile only at planes below its top. Report the tile's true top
    // elevation so the highlight lands on the top face.
    if (terrain.tryGet(tile, tileElevation) && tileElevation >= elevation)
      return TilePick{tile, world, tileElevation, true};
  }

  // No tile under the cursor: fall back to the flat ground projection so
  // callers still get a usable grid coordinate.
  const glm::vec2 ground = projection.screenToWorld(screen, 0.0f);

  return TilePick{gridCellOf(ground), ground, 0, false};
}

} // namespace sfs
