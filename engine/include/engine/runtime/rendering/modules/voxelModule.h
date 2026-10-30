#pragma once

#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"

#include "glm/glm/ext/vector_int3.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

class AssetStore;
class IBlockRegistry;
class VoxelWorld;

/**
 * Draws a VoxelWorld in a 2D game: each chunk's visible block faces (cut from
 * its block types' sprites, see voxelMesher.h) as meshes placed by the game's
 * Space2D, made when a chunk streams in or changes and kept until then; and
 * its water as see-through surfaces at each column's level.
 */
class VoxelModule : public CommandModule<RenderContext, MeshDrawCommand>
{
public:
  VoxelModule(AssetStore& assets,
              VoxelWorld& world,
              const IBlockRegistry& blocks);

  /** Water's colour, and how see-through it is (alpha). */
  void setWaterColor(const glm::vec4& color) { m_waterColor = color; }

  void computeCommands(const RenderContext& context) override;

private:
  struct ChunkMesh
  {
    MeshHandle mesh;
    MaterialHandle material;
  };
  struct IVec3Hash
  {
    std::size_t operator()(const glm::ivec3& v) const noexcept
    {
      return std::size_t(v.x) * 73856093u ^ std::size_t(v.y) * 19349663u ^
             std::size_t(v.z) * 83492791u;
    }
  };

  void remesh(const glm::ivec3& chunk, const RenderContext& context);
  void forget(const glm::ivec3& chunk, MeshLibrary& meshes);
  MaterialHandle blockMaterial(const std::string& sheet, MeshLibrary& meshes);

  AssetStore& m_assets;
  VoxelWorld& m_world;
  const IBlockRegistry& m_blocks;
  glm::vec4 m_waterColor{0.15f, 0.4f, 0.8f, 0.6f};
  std::unordered_map<glm::ivec3, std::vector<ChunkMesh>, IVec3Hash> m_chunks;
  std::unordered_map<std::string, MaterialHandle> m_materials;
  MaterialHandle m_water;
  MeshHandle m_quad;
};

} // namespace sfs
