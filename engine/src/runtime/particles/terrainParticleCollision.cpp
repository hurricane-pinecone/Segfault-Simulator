#include "engine/runtime/particles/terrainParticleCollision.h"

#include "engine/core/rendering/iTerrainSurfaceSource.h"
#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include <cstdint>

namespace sfs
{
namespace
{

// A step-up of at least this many elevation levels reads as a wall.
constexpr int kMinWallStep = 1;

glm::ivec2 floorTile(const glm::vec2& p)
{
  return {static_cast<int>(glm::floor(p.x)), static_cast<int>(glm::floor(p.y))};
}

} // namespace

ParticleHit TerrainParticleCollision::sweep(const ParticleSweep& m) const
{
  ParticleHit out;
  if (!m_terrain)
    return out;

  const float zAbs = m.toZ;
  const glm::ivec2 oldTile = floorTile(m.from);
  const glm::ivec2 newTile = floorTile(m.to);

  // Slammed into the vertical face of a tile that genuinely steps UP from the
  // one it came from (a step down or same level is not a wall -- otherwise a
  // fast droplet skimming flat ground reads every tile crossing as a wall).
  if (newTile != oldTile)
  {
    const int newTop = m_terrain->terrainHeightAt(newTile.x, newTile.y);
    const int oldTop = m_terrain->terrainHeightAt(oldTile.x, oldTile.y);

    if (newTop > oldTop && zAbs < static_cast<float>(newTop))
    {
      const int dx = newTile.x - oldTile.x;
      const int dy = newTile.y - oldTile.y;
      const std::uint8_t side = dx > 0 ? 0 : (dx < 0 ? 2 : (dy > 0 ? 1 : 3));

      // A wall stain needs a real cliff on a camera-facing face (south/east);
      // the hidden west/north faces would draw over the block, and the step
      // must be tall enough. Everything else pools as ground at the base.
      const bool visibleFace = side == 2 || side == 3;
      const bool tallEnough = (newTop - oldTop) >= kMinWallStep;

      out.hit = true;
      out.tile = newTile;
      if (visibleFace && tallEnough)
      {
        out.surface = DecalSurface::Wall;
        out.wallSide = side;
        out.wallBottom = static_cast<float>(oldTop);
        out.wallTop = static_cast<float>(newTop);
        out.pos = m.from;
        out.elevation = zAbs;
      }
      else
      {
        out.surface = DecalSurface::Ground;
        out.pos = m.from;
        out.elevation = static_cast<float>(oldTop);
      }
      return out;
    }
  }

  // Otherwise, dropped onto the ground/water beneath it.
  const int top = m_terrain->terrainHeightAt(newTile.x, newTile.y);
  if (zAbs <= static_cast<float>(top))
  {
    out.hit = true;
    out.tile = newTile;
    out.pos = m.to;
    out.elevation = static_cast<float>(top);
    if (m_terrain->isWaterAt(newTile.x, newTile.y))
    {
      out.surface = DecalSurface::Water;
      out.fadeRate = m_terrain->waterFadeRateAt(newTile.x, newTile.y);
    }
    else
    {
      out.surface = DecalSurface::Ground;
    }
  }
  return out;
}

float TerrainParticleCollision::groundElevation(glm::vec2 worldPos) const
{
  if (!m_terrain)
    return 0.0f;
  return static_cast<float>(
      m_terrain->terrainHeightAt(static_cast<int>(glm::floor(worldPos.x)),
                                 static_cast<int>(glm::floor(worldPos.y))));
}

} // namespace sfs
