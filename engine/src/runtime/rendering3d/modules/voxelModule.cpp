#include "engine/runtime/rendering3d/modules/voxelModule.h"

#include "engine/core/voxel/voxelMesher.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/rendering3d/modules/imageQuads.h"
#include "engine/runtime/voxel/voxelWorld.h"
#include "glm/glm/geometric.hpp"

namespace sfs
{

VoxelModule::VoxelModule(AssetStore& assets,
                         VoxelWorld& world,
                         const IBlockRegistry& blocks)
    : m_assets(assets), m_world(world), m_blocks(blocks)
{
}

MaterialHandle VoxelModule::blockMaterial(const std::string& sheet,
                                          MeshLibrary& meshes)
{
  auto [at, added] = m_materials.emplace(sheet, MaterialHandle{});
  if (added)
    at->second = meshes.setMaterial(
        "voxel:" + sheet, imageMaterial(m_assets, sheet, {}, AlphaMode::Mask));
  return at->second;
}

void VoxelModule::forget(const glm::ivec3& chunk, MeshLibrary& meshes)
{
  const auto it = m_chunks.find(chunk);
  if (it == m_chunks.end())
    return;
  for (const ChunkMesh& part : it->second)
    meshes.remove(part.mesh);
  m_chunks.erase(it);
}

void VoxelModule::remesh(const glm::ivec3& chunk,
                         const RenderContext3D& context)
{
  MeshLibrary& meshes = *context.meshes;
  const Space2D& space = *context.space2D;
  forget(chunk, meshes);
  std::vector<ChunkMesh>& parts = m_chunks[chunk];
  for (const VoxelMeshSlice& slice : meshChunk(chunk, m_world, m_blocks))
  {
    if (slice.vertices.empty() || !slice.textureId)
      continue;
    // The faces, from the game's grid into the world (their triangles turned
    // the other way if the grid is the world's mirror image).
    MeshData mesh;
    mesh.vertices.reserve(slice.vertices.size());
    for (const GeometryVertex& vertex : slice.vertices)
    {
      MeshVertex out;
      out.position = space.place(vertex.worldPos, vertex.ground);
      out.normal = glm::normalize(space.direction(vertex.normal));
      out.uv = vertex.uv;
      mesh.vertices.push_back(out);
    }
    const bool flip = space.mirrored();
    for (uint32_t i = 0; i + 2 < mesh.vertices.size(); i += 3)
      mesh.indices.insert(
          mesh.indices.end(), {i, flip ? i + 2 : i + 1, flip ? i + 1 : i + 2});
    const MeshHandle handle = meshes.add(mesh);
    if (handle.valid())
      parts.push_back({handle, blockMaterial(*slice.textureId, meshes)});
  }
}

void VoxelModule::computeCommands(const RenderContext3D& context)
{
  flush();
  if (!context.meshes || !context.space2D)
    return;
  MeshLibrary& meshes = *context.meshes;

  // Chunks streamed out go; those that changed (or streamed in) are made
  // again; the rest are kept as they were.
  std::vector<glm::ivec3> gone;
  for (const auto& [chunk, parts] : m_chunks)
    if (!m_world.hasChunk(chunk))
      gone.push_back(chunk);
  for (const glm::ivec3& chunk : gone)
    forget(chunk, meshes);
  for (const glm::ivec3& chunk : m_world.dirtyChunks())
    remesh(chunk, context);
  m_world.clearDirty();

  for (const auto& [chunk, parts] : m_chunks)
    for (const ChunkMesh& part : parts)
    {
      MeshDrawCommand command;
      command.mesh = part.mesh;
      command.materials = {part.material};
      command.instances = {MeshInstance{}};
      m_commands.push_back(std::move(command));
    }

  // Water: a see-through surface over each column, at its level.
  std::vector<WaterColumn> columns;
  m_world.collectWaterColumns(columns);
  if (columns.empty())
    return;
  if (!m_quad.valid())
    m_quad = imageQuad(meshes);
  if (!m_water.valid())
    m_water = meshes.setMaterial(
        "voxel:water",
        imageMaterial(m_assets, "white_pixel", {}, AlphaMode::Blend));
  const Space2D& space = *context.space2D;
  MeshDrawCommand water;
  water.pass = RenderPass3D::Transparent;
  water.mesh = m_quad;
  water.materials = {m_water};
  for (const WaterColumn& column : columns)
  {
    const glm::vec2 corner(column.tile);
    const glm::vec3 origin = space.place(corner, column.surfaceLevel);
    MeshInstance surface;
    const glm::vec3 along =
        space.place(corner + glm::vec2(1, 0), column.surfaceLevel) - origin;
    const glm::vec3 across =
        space.place(corner + glm::vec2(0, 1), column.surfaceLevel) - origin;
    surface.model[0] = glm::vec4(along, 0.0f);
    surface.model[1] = glm::vec4(across, 0.0f);
    surface.model[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    surface.model[3] = glm::vec4(origin + (along + across) * 0.5f, 1.0f);
    surface.tint = m_waterColor;
    water.instances.push_back(surface);
  }
  m_commands.push_back(std::move(water));
}

} // namespace sfs
