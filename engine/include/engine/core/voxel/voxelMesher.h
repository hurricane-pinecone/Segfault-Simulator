#pragma once

#include "engine/core/rendering/vertices.h"
#include "engine/core/voxel/voxelView.h"
#include "glm/glm/ext/vector_int3.hpp"
#include <string>
#include <vector>

namespace sfs
{

// A run of triangles sharing one material (texture + surface effect).
struct VoxelMeshSlice
{
  const std::string* textureId = nullptr;
  SurfaceEffect::Type effect = SurfaceEffect::Type::None;
  // World-space vertices: worldPos + ground (elevation levels) + uv + normal +
  // z (painter key). `position` (screen) is left zero -- the render module
  // fills it via the projection at emit time, so cached meshes survive camera
  // moves.
  std::vector<GeometryVertex> vertices;
};

// Mesh one chunk into per-material slices. For each solid voxel it emits only
// the camera-facing +x / +y / +z faces, and only when the neighbour on that
// side is non-opaque -- so interior faces and the three away-facing faces never
// exist. Pure: no projection, no GL, no asset store (uv comes from each block
// type's normalised uvRect), so it builds and unit-tests under the core-only
// library.
std::vector<VoxelMeshSlice> meshChunk(glm::ivec3 chunkCoord,
                                      const IVoxelView& view,
                                      const IBlockRegistry& registry);

} // namespace sfs
