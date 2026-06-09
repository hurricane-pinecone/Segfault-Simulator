#pragma once

#include "engine/core/voxel/tinyVoxelChunk.h"
#include "glm/glm/ext/vector_int3.hpp"

namespace sfs
{

// Fills tiny-voxel chunks for the streaming TinyVoxelWorld. The game supplies
// the implementation (terrain shape + colours); the world owns
// storage/streaming and calls this for each chunk it loads. Pure data in,
// voxels out -- no engine deps.
class ITinyVoxelGenerator
{
public:
  virtual ~ITinyVoxelGenerator() = default;

  // Fill `out` (already all-air) for the chunk at `chunkCoord` (in chunks). The
  // chunk's world-voxel origin is chunkCoord * kTinyChunkSize.
  virtual void generate(glm::ivec3 chunkCoord, TinyVoxelChunk& out) const = 0;

  // Sea level in world voxels: columns whose surface sits below it read as
  // water.
  virtual int seaLevel() const = 0;
};

} // namespace sfs
