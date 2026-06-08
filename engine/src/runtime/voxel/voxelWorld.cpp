#include "engine/runtime/voxel/voxelWorld.h"

#include "engine/core/components/cameraComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/util/profiling.h"
#include "glm/glm/common.hpp"

#include <vector>

namespace sfs
{

namespace
{
// A writer that drops a generator's blocks into one chunk (clamped to bounds).
struct ChunkWriter : IChunkWriter
{
  VoxelChunk* chunk = nullptr;
  void set(int lx, int ly, int lz, BlockId id) override
  {
    if (lx >= 0 && lx < kChunkSize && ly >= 0 && ly < kChunkSize && lz >= 0 &&
        lz < kChunkSize)
      chunk->set(lx, ly, lz, id);
  }
};
} // namespace

int VoxelWorld::floorDiv(int a, int b)
{
  int q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    --q;
  return q;
}

glm::ivec3 VoxelWorld::chunkOf(int x, int y, int z)
{
  return {floorDiv(x, kChunkSize),
          floorDiv(y, kChunkSize),
          floorDiv(z, kChunkSize)};
}

VoxelWorld::VoxelWorld(int minBlockZ, int maxBlockZ, int radiusTiles)
    : m_minChunkZ(floorDiv(minBlockZ, kChunkSize)),
      m_maxChunkZ(floorDiv(maxBlockZ - 1, kChunkSize)),
      m_radiusTiles(radiusTiles), m_lastFocus(0, 0)
{
}

BlockId VoxelWorld::blockAt(int x, int y, int z) const
{
  const glm::ivec3 c = chunkOf(x, y, z);
  const auto it = m_chunks.find(c);
  if (it == m_chunks.end())
    return kAirBlock;
  return it->second->at(
      x - c.x * kChunkSize, y - c.y * kChunkSize, z - c.z * kChunkSize);
}

bool VoxelWorld::isSolid(BlockId id) const
{
  if (id == kAirBlock)
    return false;
  return m_registry ? m_registry->type(id).solid : true;
}

bool VoxelWorld::isLiquid(BlockId id) const
{
  return m_registry && id != kAirBlock && m_registry->type(id).liquid;
}

int VoxelWorld::topLevel(BlockId id, int z) const
{
  const bool slab =
      m_registry && m_registry->type(id).shape == BlockShape::Slab;
  return slab ? z * kLevelsPerBlock + 1 : (z + 1) * kLevelsPerBlock;
}

int VoxelWorld::terrainHeightAt(int tileX, int tileY) const
{
  const auto it = m_surfaceTop.find({tileX, tileY});
  if (it == m_surfaceTop.end())
    return m_minChunkZ * kChunkSize * kLevelsPerBlock; // world floor
  return it->second;
}

bool VoxelWorld::isWaterAt(int tileX, int tileY) const
{
  const auto it = m_waterTop.find({tileX, tileY});
  if (it == m_waterTop.end())
    return false;
  return (it->second + 1) * kLevelsPerBlock > terrainHeightAt(tileX, tileY);
}

void VoxelWorld::collectWaterColumns(std::vector<WaterColumn>& out) const
{
  for (const auto& [tile, waterZ] : m_waterTop)
  {
    // Skip stale entries left by unloaded columns -- only draw water in the
    // currently streamed window.
    if (tile.x < m_lastFocus.x - m_radiusTiles ||
        tile.x > m_lastFocus.x + m_radiusTiles ||
        tile.y < m_lastFocus.y - m_radiusTiles ||
        tile.y > m_lastFocus.y + m_radiusTiles)
      continue;

    const int surfaceLevel = (waterZ + 1) * kLevelsPerBlock;
    const int floorLevel = terrainHeightAt(tile.x, tile.y);
    if (surfaceLevel > floorLevel)
      out.push_back(WaterColumn{tile, surfaceLevel, floorLevel});
  }
}

void VoxelWorld::markDirty(const glm::ivec3& coord)
{
  if (hasChunk(coord))
    m_dirty.insert(coord);
}

void VoxelWorld::setBlock(int x, int y, int z, BlockId id)
{
  const glm::ivec3 c = chunkOf(x, y, z);
  const auto it = m_chunks.find(c);
  if (it == m_chunks.end())
    return; // editing only loaded terrain

  it->second->set(
      x - c.x * kChunkSize, y - c.y * kChunkSize, z - c.z * kChunkSize, id);

  if (isSolid(id))
  {
    int& top = m_surfaceTop[{x, y}];
    top = glm::max(top, topLevel(id, z));
  }
  else if (isLiquid(id))
  {
    int& top = m_waterTop[{x, y}];
    top = glm::max(top, z);
  }

  // The block's own chunk, plus the chunks whose +x/+y/+z face abuts it.
  markDirty(c);
  markDirty(chunkOf(x - 1, y, z));
  markDirty(chunkOf(x, y - 1, z));
  markDirty(chunkOf(x, y, z - 1));
}

void VoxelWorld::loadChunk(const glm::ivec3& coord)
{
  auto chunk = std::make_unique<VoxelChunk>();

  if (m_generator)
  {
    ChunkWriter writer;
    writer.chunk = chunk.get();
    m_generator->generate(coord, writer);
  }

  // Record the topmost solid (walkable floor) and topmost liquid (water
  // surface) for each column this chunk covers.
  for (int ly = 0; ly < kChunkSize; ++ly)
    for (int lx = 0; lx < kChunkSize; ++lx)
    {
      const int wx = coord.x * kChunkSize + lx;
      const int wy = coord.y * kChunkSize + ly;
      bool gotSolid = false;
      bool gotWater = false;
      for (int lz = kChunkSize - 1; lz >= 0 && !(gotSolid && gotWater); --lz)
      {
        const BlockId id = chunk->at(lx, ly, lz);
        if (id == kAirBlock)
          continue;
        const int wz = coord.z * kChunkSize + lz;
        if (!gotSolid && isSolid(id))
        {
          int& top = m_surfaceTop[{wx, wy}];
          top = glm::max(top, topLevel(id, wz));
          gotSolid = true;
        }
        if (!gotWater && isLiquid(id))
        {
          int& top = m_waterTop[{wx, wy}];
          top = glm::max(top, wz);
          gotWater = true;
        }
      }
    }

  m_chunks.emplace(coord, std::move(chunk));
  // New chunk + the chunks whose +face now abuts it.
  markDirty(coord);
  markDirty({coord.x - 1, coord.y, coord.z});
  markDirty({coord.x, coord.y - 1, coord.z});
  markDirty({coord.x, coord.y, coord.z - 1});
}

void VoxelWorld::streamAround(const glm::ivec2& focusTile)
{
  ZoneScopedN("VoxelWorld::streamAround");

  const int minCX = floorDiv(focusTile.x - m_radiusTiles, kChunkSize);
  const int maxCX = floorDiv(focusTile.x + m_radiusTiles, kChunkSize);
  const int minCY = floorDiv(focusTile.y - m_radiusTiles, kChunkSize);
  const int maxCY = floorDiv(focusTile.y + m_radiusTiles, kChunkSize);

  for (int cz = m_minChunkZ; cz <= m_maxChunkZ; ++cz)
    for (int cy = minCY; cy <= maxCY; ++cy)
      for (int cx = minCX; cx <= maxCX; ++cx)
      {
        const glm::ivec3 coord{cx, cy, cz};
        if (!hasChunk(coord))
          loadChunk(coord);
      }

  // Unload columns outside the (x,y) window.
  std::vector<glm::ivec3> drop;
  for (const auto& [coord, chunk] : m_chunks)
    if (coord.x < minCX || coord.x > maxCX || coord.y < minCY ||
        coord.y > maxCY)
      drop.push_back(coord);

  for (const glm::ivec3& coord : drop)
  {
    m_chunks.erase(coord);
    m_dirty.erase(coord);
  }
}

void VoxelWorld::update(double /*deltaTime*/)
{
  auto cameras = registry->view<CameraComponent, TransformComponent>();
  if (cameras.empty())
    return;

  const auto& transform = cameras.front().getComponent<TransformComponent>();
  const auto& camera = cameras.front().getComponent<CameraComponent>();
  const glm::vec2 focus = transform.position + camera.offset;
  const glm::ivec2 focusTile{static_cast<int>(glm::floor(focus.x)),
                             static_cast<int>(glm::floor(focus.y))};

  if (m_streamed && focusTile == m_lastFocus)
    return;

  m_lastFocus = focusTile;
  m_streamed = true;
  streamAround(focusTile);
}

} // namespace sfs
