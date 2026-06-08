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
bool inChunk(int lx, int ly, int lz)
{
  return lx >= 0 && lx < kChunkSize && ly >= 0 && ly < kChunkSize && lz >= 0 &&
         lz < kChunkSize;
}

// A writer that drops a generator's blocks + water into one chunk pair.
struct ChunkWriter : IChunkWriter
{
  VoxelChunk* chunk = nullptr;
  WaterChunk* waterChunk = nullptr;
  void set(int lx, int ly, int lz, BlockId id) override
  {
    if (inChunk(lx, ly, lz))
      chunk->set(lx, ly, lz, id);
  }
  void setWater(int lx, int ly, int lz, std::uint16_t amount) override
  {
    if (inChunk(lx, ly, lz))
      waterChunk->set(lx, ly, lz, amount);
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

int VoxelWorld::topLevel(BlockId id, int z) const
{
  const bool slab =
      m_registry && m_registry->type(id).shape == BlockShape::Slab;
  return slab ? z * kLevelsPerBlock + 1 : (z + 1) * kLevelsPerBlock;
}

void VoxelWorld::recomputeSurfaceTop(int x, int y)
{
  const int top = (m_maxChunkZ + 1) * kChunkSize - 1;
  const int floor = m_minChunkZ * kChunkSize;
  for (int z = top; z >= floor; --z)
  {
    const BlockId id = blockAt(x, y, z);
    if (id != kAirBlock && isSolid(id))
    {
      m_surfaceTop[{x, y}] = topLevel(id, z);
      return;
    }
  }
  m_surfaceTop.erase({x, y});
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
  const auto it = m_waterTopCell.find({tileX, tileY});
  if (it == m_waterTopCell.end())
    return false;
  const glm::ivec3 top{tileX, tileY, it->second};
  return surface(top) > static_cast<float>(terrainHeightAt(tileX, tileY));
}

void VoxelWorld::collectWaterColumns(std::vector<WaterColumn>& out) const
{
  for (const auto& [tile, topZ] : m_waterTopCell)
  {
    // Only the currently streamed window.
    if (tile.x < m_lastFocus.x - m_radiusTiles ||
        tile.x > m_lastFocus.x + m_radiusTiles ||
        tile.y < m_lastFocus.y - m_radiusTiles ||
        tile.y > m_lastFocus.y + m_radiusTiles)
      continue;

    const glm::ivec3 top{tile.x, tile.y, topZ};
    if (water(top) <= 0)
      continue;
    const float surfaceLevel = surface(top);
    const float floorLevel =
        static_cast<float>(terrainHeightAt(tile.x, tile.y));
    if (surfaceLevel > floorLevel)
      out.push_back(WaterColumn{tile, surfaceLevel, floorLevel});
  }
}

// --- Water grid + sim ------------------------------------------------------

int VoxelWorld::water(const glm::ivec3& c) const
{
  const glm::ivec3 cc = chunkOf(c.x, c.y, c.z);
  const auto it = m_waterChunks.find(cc);
  if (it == m_waterChunks.end())
    return 0;
  return it->second.at(c.x - cc.x * kChunkSize,
                       c.y - cc.y * kChunkSize,
                       c.z - cc.z * kChunkSize);
}

int VoxelWorld::capacity(const glm::ivec3& c) const
{
  if (!m_registry)
    return 0;
  // An unloaded block chunk is a barrier -- water doesn't flow off the streamed
  // edge or out of the world's z range.
  if (m_chunks.find(chunkOf(c.x, c.y, c.z)) == m_chunks.end())
    return 0;
  return cellWaterCapacity(blockAt(c.x, c.y, c.z), *m_registry);
}

float VoxelWorld::surface(const glm::ivec3& c) const
{
  if (!m_registry)
    return static_cast<float>(c.z * kLevelsPerBlock);
  return cellWaterSurface(c.z, blockAt(c.x, c.y, c.z), *m_registry, water(c));
}

void VoxelWorld::setWater(const glm::ivec3& c, int amount)
{
  amount = glm::clamp(amount, 0, 65535);
  const glm::ivec3 cc = chunkOf(c.x, c.y, c.z);
  const int lx = c.x - cc.x * kChunkSize;
  const int ly = c.y - cc.y * kChunkSize;
  const int lz = c.z - cc.z * kChunkSize;

  if (amount == 0)
  {
    const auto it = m_waterChunks.find(cc);
    if (it != m_waterChunks.end())
      it->second.set(lx, ly, lz, 0);
  }
  else
  {
    m_waterChunks[cc].set(lx, ly, lz, static_cast<WaterAmount>(amount));
  }
  updateWaterTop(c, amount);
}

void VoxelWorld::updateWaterTop(const glm::ivec3& c, int amount)
{
  const glm::ivec2 col{c.x, c.y};
  const auto it = m_waterTopCell.find(col);
  if (amount > 0)
  {
    if (it == m_waterTopCell.end() || c.z > it->second)
      m_waterTopCell[col] = c.z;
    return;
  }
  // Emptied a cell: if it was the column's top, find the next water below.
  if (it == m_waterTopCell.end() || c.z != it->second)
    return;
  const int floor = m_minChunkZ * kChunkSize;
  for (int z = c.z - 1; z >= floor; --z)
    if (water({c.x, c.y, z}) > 0)
    {
      it->second = z;
      return;
    }
  m_waterTopCell.erase(it);
}

void VoxelWorld::activate(const glm::ivec3& c)
{
  m_activeWater.insert(c);
  m_activeWater.insert({c.x + 1, c.y, c.z});
  m_activeWater.insert({c.x - 1, c.y, c.z});
  m_activeWater.insert({c.x, c.y + 1, c.z});
  m_activeWater.insert({c.x, c.y - 1, c.z});
  m_activeWater.insert({c.x, c.y, c.z + 1});
  m_activeWater.insert({c.x, c.y, c.z - 1});
}

void VoxelWorld::addWater(int x, int y, int z, int amount)
{
  const glm::ivec3 c{x, y, z};
  const int room = capacity(c) - water(c);
  const int add = glm::min(glm::max(amount, 0), glm::max(room, 0));
  if (add <= 0)
    return;
  setWater(c, water(c) + add);
  activate(c);
}

void VoxelWorld::simulateWater(double deltaTime)
{
  ZoneScopedN("VoxelWorld::simulateWater");

  constexpr double kTick = 1.0 / 15.0;
  m_simAccum += deltaTime;

  int ticks = 0;
  while (m_simAccum >= kTick && ticks < 4)
  {
    m_simAccum -= kTick;
    ++ticks;
    if (m_activeWater.empty())
      break;

    const std::vector<glm::ivec3> active(
        m_activeWater.begin(), m_activeWater.end());
    std::vector<glm::ivec3> next;
    stepWater(active, *this, next);
    m_activeWater.clear();
    m_activeWater.insert(next.begin(), next.end());
  }

  if (m_activeWater.empty())
    m_simAccum = 0.0; // don't bank time while idle
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

  // The edit may raise OR lower the column's surface, so rescan it.
  recomputeSurfaceTop(x, y);

  // Changing a block changes water capacity here -- wake the water at and above
  // it so it can flow into a dug hole or be displaced by a placed block.
  activate({x, y, z});
  activate({x, y, z + 1});

  // The block's own chunk, plus the chunks whose +x/+y/+z face abuts it.
  markDirty(c);
  markDirty(chunkOf(x - 1, y, z));
  markDirty(chunkOf(x, y - 1, z));
  markDirty(chunkOf(x, y, z - 1));
}

void VoxelWorld::loadChunk(const glm::ivec3& coord)
{
  auto chunk = std::make_unique<VoxelChunk>();
  WaterChunk waterChunk;

  if (m_generator)
  {
    ChunkWriter writer;
    writer.chunk = chunk.get();
    writer.waterChunk = &waterChunk;
    m_generator->generate(coord, writer);
  }

  // Record the topmost solid (walkable floor) and topmost water cell for each
  // column this chunk covers. Generated water is assumed settled (the generator
  // fills to a flat level), so it is NOT woken -- a loaded sea costs nothing.
  for (int ly = 0; ly < kChunkSize; ++ly)
    for (int lx = 0; lx < kChunkSize; ++lx)
    {
      const int wx = coord.x * kChunkSize + lx;
      const int wy = coord.y * kChunkSize + ly;
      bool gotSolid = false;
      bool gotWater = false;
      for (int lz = kChunkSize - 1; lz >= 0 && !(gotSolid && gotWater); --lz)
      {
        const int wz = coord.z * kChunkSize + lz;
        if (!gotWater && waterChunk.at(lx, ly, lz) > 0)
        {
          const glm::ivec2 col{wx, wy};
          const auto wit = m_waterTopCell.find(col);
          m_waterTopCell[col] =
              wit == m_waterTopCell.end() ? wz : glm::max(wit->second, wz);
          gotWater = true;
        }
        const BlockId id = chunk->at(lx, ly, lz);
        if (!gotSolid && id != kAirBlock && isSolid(id))
        {
          int& sTop = m_surfaceTop[{wx, wy}];
          sTop = glm::max(sTop, topLevel(id, wz));
          gotSolid = true;
        }
      }
    }

  if (!waterChunk.empty())
    m_waterChunks.emplace(coord, std::move(waterChunk));
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

void VoxelWorld::update(double deltaTime)
{
  // The fluid sim runs every frame regardless of camera movement.
  simulateWater(deltaTime);

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
