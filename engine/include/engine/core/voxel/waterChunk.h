#pragma once

#include "engine/core/voxel/voxelChunk.h"
#include <array>
#include <cstdint>

namespace sfs
{

// Water amount in one cell. A full air cell holds kWaterFull; a slab cell holds
// half (its lower level is solid); a cube cell holds none. Stored as a per-cell
// scalar in a layer parallel to the block grid -- water is NOT a block, so it
// can rest at any height (meeting half-block terrain) and be simulated.
using WaterAmount = std::uint16_t;
constexpr WaterAmount kWaterFull = 256;

struct WaterChunk
{
  std::array<WaterAmount, kChunkSize * kChunkSize * kChunkSize> water{};

  WaterAmount at(int lx, int ly, int lz) const
  {
    return water[VoxelChunk::index(lx, ly, lz)];
  }
  void set(int lx, int ly, int lz, WaterAmount amount)
  {
    water[VoxelChunk::index(lx, ly, lz)] = amount;
  }
  bool empty() const
  {
    for (WaterAmount w : water)
      if (w != 0)
        return false;
    return true;
  }
};

} // namespace sfs
