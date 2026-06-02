#pragma once

namespace sfs
{

// Authoritative terrain elevation, in levels, at a world tile.
//
// The point-light occlusion heightmap needs terrain heights that are complete
// the instant the camera moves. Reading them from the ECS (one entity per tile)
// can't deliver that: entity creation is deferred a frame, so on a frame where
// terrain is still streaming in, the leading-edge tiles are absent and their
// occluders momentarily vanish -- the occluded area flashes bright. A height
// source answers from the generator's own deterministic data instead, so the
// heightmap window is always hole-free regardless of entity lifetime.
class ITerrainHeightSource
{
public:
  virtual ~ITerrainHeightSource() = default;

  virtual int terrainHeightAt(int tileX, int tileY) const = 0;
};

} // namespace sfs
