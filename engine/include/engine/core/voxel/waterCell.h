#pragma once

#include "engine/core/voxel/voxelView.h"
#include "engine/core/voxel/waterChunk.h"

namespace sfs
{

// How many of a cell's kLevelsPerBlock levels its solid block occupies (cube =
// the whole cell, slab = the lower level).
inline int blockSolidLevels(const BlockType& bt)
{
  return bt.shape == BlockShape::Slab ? 1 : kLevelsPerBlock;
}

// Levels of solid in a cell holding block `id` (air / non-solid block = none).
inline int cellSolidLevels(BlockId id, const IBlockRegistry& registry)
{
  if (id == kAirBlock)
    return 0;
  const BlockType& bt = registry.type(id);
  return bt.solid ? blockSolidLevels(bt) : 0;
}

// Max water a cell can hold: the empty fraction of the cell. Air = kWaterFull,
// slab = half, cube = 0.
inline int cellWaterCapacity(BlockId id, const IBlockRegistry& registry)
{
  const int empty = kLevelsPerBlock - cellSolidLevels(id, registry);
  return kWaterFull * empty / kLevelsPerBlock;
}

// Water surface height (elevation levels) for `amount` water in cell z holding
// block `id`: it rests on top of the cell's solid and fills the empty space.
inline float
cellWaterSurface(int z, BlockId id, const IBlockRegistry& registry, int amount)
{
  const int solid = cellSolidLevels(id, registry);
  const int emptyLevels = kLevelsPerBlock - solid;
  const int capacity = kWaterFull * emptyLevels / kLevelsPerBlock;
  const float bottom = static_cast<float>(z * kLevelsPerBlock + solid);
  const float frac =
      capacity > 0 ? static_cast<float>(amount) / static_cast<float>(capacity)
                   : 0.0f;
  return bottom + frac * static_cast<float>(emptyLevels);
}

} // namespace sfs
