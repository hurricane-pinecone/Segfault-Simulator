#pragma once

#include "engine/core/voxel/voxelMesher.h"
#include "engine/runtime/rendering/commands/commands.h"
#include "engine/runtime/rendering/isometricRenderContext.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/voxel/voxelWorld.h"
#include "glm/glm/ext/vector_int3.hpp"
#include <unordered_map>
#include <vector>

namespace sfs
{

// Render module that draws a VoxelWorld as real block-face geometry, the voxel
// counterpart to BlockGeometry. It reads the world (data authority) through the
// injected reference -- it does not own the voxels -- meshes only the world's
// dirty chunks (caching the rest, since voxels are far too heavy to rebuild
// every frame), then projects the cached world-space meshes and emits one
// GeometryCommand per material. Registering it suppresses billboard tiles, same
// as BlockGeometry.
class VoxelTerrain
    : public CommandModule<IsometricRenderContext, GeometryCommand>
{
public:
  // Non-const: the module acks the world's dirty chunks (clearDirty) after
  // meshing them. It never edits voxel data.
  void setWorld(VoxelWorld& world, const IBlockRegistry& registry)
  {
    m_world = &world;
    m_registry = &registry;
  }

  bool providesTerrainGeometry() const override { return true; }

  void computeCommands(const IsometricRenderContext& context) override;

private:
  VoxelWorld* m_world = nullptr;
  const IBlockRegistry* m_registry = nullptr;

  // Per-chunk meshes in world space; remeshed only when the chunk is dirty.
  std::unordered_map<glm::ivec3, std::vector<VoxelMeshSlice>, IVec3Hash>
      m_meshes;
};

} // namespace sfs
