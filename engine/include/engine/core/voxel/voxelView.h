#pragma once

#include "engine/core/voxel/blockType.h"
#include "glm/glm/ext/vector_int3.hpp"

namespace sfs
{

// Read access to voxels in world (block) coordinates, across chunk boundaries.
// The mesher samples a voxel's +x/+y/+z neighbours through this, so a voxel on
// a chunk edge correctly sees into the adjacent chunk.
class IVoxelView
{
public:
  virtual ~IVoxelView() = default;
  virtual BlockId blockAt(int x, int y, int z) const = 0;
};

// Maps a block id to its static type. Game-owned (it knows the textures),
// engine-queried.
class IBlockRegistry
{
public:
  virtual ~IBlockRegistry() = default;
  virtual const BlockType& type(BlockId id) const = 0;
};

// Sink a generator writes a chunk's voxels into (chunk-local coordinates).
class IChunkWriter
{
public:
  virtual ~IChunkWriter() = default;
  virtual void set(int lx, int ly, int lz, BlockId id) = 0;
};

// Produces world content. The game implements it (noise, structures, ...); the
// engine calls it once per chunk as terrain streams in.
class IVoxelGenerator
{
public:
  virtual ~IVoxelGenerator() = default;
  virtual void generate(glm::ivec3 chunkCoord, IChunkWriter& out) const = 0;
};

} // namespace sfs
