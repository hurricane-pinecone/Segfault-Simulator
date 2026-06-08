#pragma once

#include "engine/core/voxel/blockType.h"
#include <array>

namespace sfs
{

// Cubic chunk edge in voxels. A chunk is a dense block-id grid; all-air chunks
// are simply never allocated by the world store.
constexpr int kChunkSize = 32;

struct VoxelChunk
{
  std::array<BlockId, kChunkSize * kChunkSize * kChunkSize> blocks{}; // all air

  static int index(int lx, int ly, int lz)
  {
    return (lz * kChunkSize + ly) * kChunkSize + lx;
  }

  BlockId at(int lx, int ly, int lz) const { return blocks[index(lx, ly, lz)]; }
  void set(int lx, int ly, int lz, BlockId id)
  {
    blocks[index(lx, ly, lz)] = id;
  }
  bool empty() const
  {
    for (BlockId b : blocks)
      if (b != kAirBlock)
        return false;
    return true;
  }
};

} // namespace sfs
