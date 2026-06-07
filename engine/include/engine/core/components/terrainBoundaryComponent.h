#pragma once

namespace sfs
{

/**
 * Per-tile record of which sides drop off to lower terrain, used to build the
 * exposed cliff faces. Each *Exposed flags an open side; the matching
 * *BottomElevation is how far that side drops. dirty marks the tile for a
 * boundary recompute. Filled by the terrain systems, not set by hand.
 */
struct TerrainBoundaryComponent
{
  bool westExposed = false;
  bool eastExposed = false;
  bool northExposed = false;
  bool southExposed = false;

  int westBottomElevation = 0;
  int eastBottomElevation = 0;
  int northBottomElevation = 0;
  int southBottomElevation = 0;

  bool dirty = true;
};

} // namespace sfs
