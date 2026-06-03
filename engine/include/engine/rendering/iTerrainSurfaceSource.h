#pragma once

#include "engine/rendering/iTerrainHeightSource.h"

namespace sfs
{

// Terrain queries needed to collide particles with the world and decide what
// surface a landed particle stains. Extends ITerrainHeightSource (height) with
// water awareness, so anything that is a surface source is also a height source.
class ITerrainSurfaceSource : public ITerrainHeightSource
{
public:
  // True if the tile's surface is water (elevation at/under the water level).
  virtual bool isWaterAt(int tileX, int tileY) const = 0;

  // Per-second alpha decay for blood that lands on this water tile. Lets small
  // bodies (puddles, ~0) keep blood while large bodies (lakes, higher) wash it
  // away. Default: a moderate, uniform rate; override per tile/body for variety.
  virtual float waterFadeRateAt(int /*tileX*/, int /*tileY*/) const
  {
    return 0.5f;
  }
};

} // namespace sfs
