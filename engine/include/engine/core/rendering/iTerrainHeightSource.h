#pragma once

namespace sfs
{

// Where an actor would stand at a tile, and whether it can: `floor` is the top
// (in levels) it rests on; `blocked` is true when a wall/low ceiling stops it
// moving there (solid rises into its body above the reachable floor).
struct WalkableFloor
{
  int floor = 0;
  bool blocked = false;
};

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

  // The floor an actor at `fromLevel` would stand on at this tile, given it can
  // climb up to `maxClimb` levels and needs `clearance` levels of headroom. For
  // a flat heightfield this is just the surface (blocked when it rises more
  // than maxClimb). A voxel world picks the nearest reachable floor -- the
  // surface, a step, or a cave floor when the actor is below ground -- and
  // blocks when a wall/ceiling occupies its body, which is what lets actors
  // walk through caves while still being stopped by cliffs.
  virtual WalkableFloor walkableFloor(int tileX,
                                      int tileY,
                                      int fromLevel,
                                      int maxClimb,
                                      int /*clearance*/) const
  {
    const int surface = terrainHeightAt(tileX, tileY);
    return {surface, surface - fromLevel > maxClimb};
  }
};

} // namespace sfs
