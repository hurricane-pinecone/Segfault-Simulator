#include "engine/runtime/rendering/modules/voxelTerrain.h"

#include "engine/core/util/profiling.h"

#include <string>
#include <unordered_map>

namespace sfs
{

void VoxelTerrain::computeCommands(const IsometricRenderContext& context)
{
  ZoneScopedN("VoxelTerrain::computeCommands");

  flush();

  if (!context.projection || m_world == nullptr || m_registry == nullptr)
    return;

  // Drop cached meshes for chunks the world has streamed out.
  for (auto it = m_meshes.begin(); it != m_meshes.end();)
  {
    if (!m_world->hasChunk(it->first))
      it = m_meshes.erase(it);
    else
      ++it;
  }

  // Remesh only what changed; the rest stays cached across frames/camera moves.
  for (const glm::ivec3& coord : m_world->dirtyChunks())
    m_meshes[coord] = meshChunk(coord, *m_world, *m_registry);
  m_world->clearDirty();

  // Cull chunks to the on-screen diamond. The world streams a square radius
  // wide enough to cover the screen corners + the shadow window, but the screen
  // is a diamond in tile space, so most of that square's corners are
  // off-screen. Draw only chunks whose footprint overlaps the diamond |dx-dy|
  // <= A and |dx+dy| <= B (camera-relative), A/B = screen half-size / per-tile
  // iso step
  // (+ a margin for elevation and the chunk grain).
  const auto& proj = *context.projection;
  const float kx =
      static_cast<float>(proj.tileWidth) * proj.worldScale * proj.zoom * 0.5f;
  const float ky =
      static_cast<float>(proj.tileHeight) * proj.worldScale * proj.zoom * 0.5f;
  const float halfA = kx > 0.01f ? proj.screenCenter.x / kx : 1.0e9f;
  const float halfB = ky > 0.01f ? proj.screenCenter.y / ky + 10.0f : 1.0e9f;
  const glm::ivec2 camTile =
      context.gridCellOf(context.screenToWorld(proj.screenCenter, 0.0f));
  constexpr float kPad = 2.0f;

  const auto chunkVisible = [&](const glm::ivec3& c)
  {
    const int bx = c.x * kChunkSize - camTile.x;
    const int by = c.y * kChunkSize - camTile.y;
    const int hi = kChunkSize - 1;
    const float uMin = static_cast<float>(bx - (by + hi));
    const float uMax = static_cast<float>((bx + hi) - by);
    const float vMin = static_cast<float>(bx + by);
    const float vMax = static_cast<float>((bx + hi) + (by + hi));
    if (uMax < -halfA - kPad || uMin > halfA + kPad)
      return false;
    if (vMax < -halfB - kPad || vMin > halfB + kPad)
      return false;
    return true;
  };

  // Merge cached chunk meshes per material, projecting world -> screen (cheap;
  // the costly meshing was cached). One GeometryCommand per material.
  struct Bucket
  {
    const std::string* textureId = nullptr;
    SurfaceEffect::Type effect = SurfaceEffect::Type::None;
    std::vector<GeometryVertex> vertices;
  };
  std::unordered_map<std::string, Bucket> buckets;

  for (const auto& [coord, slices] : m_meshes)
  {
    if (!chunkVisible(coord))
      continue;

    for (const VoxelMeshSlice& slice : slices)
    {
      if (slice.vertices.empty())
        continue;

      const std::string key = *slice.textureId + '|' +
                              std::to_string(static_cast<int>(slice.effect));
      Bucket& bucket = buckets[key];
      bucket.textureId = slice.textureId;
      bucket.effect = slice.effect;

      const std::size_t base = bucket.vertices.size();
      bucket.vertices.insert(
          bucket.vertices.end(), slice.vertices.begin(), slice.vertices.end());
      for (std::size_t i = base; i < bucket.vertices.size(); ++i)
      {
        GeometryVertex& v = bucket.vertices[i];
        v.position = context.worldToScreen(v.worldPos, v.ground);
      }
    }
  }

  m_commands.reserve(buckets.size());
  for (auto& [key, bucket] : buckets)
  {
    GeometryCommand command;
    command.order = RenderOrder{RenderPass::Terrain, 0, 0};
    command.textureId = bucket.textureId;
    command.type = bucket.effect;
    command.vertices = std::move(bucket.vertices);
    m_commands.push_back(std::move(command));
  }
}

} // namespace sfs
