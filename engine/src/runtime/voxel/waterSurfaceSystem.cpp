#include "engine/runtime/voxel/waterSurfaceSystem.h"

#include "engine/core/util/profiling.h"
#include "engine/core/voxel/tinyVoxelChunk.h" // kTinyChunkSize
#include "glm/glm/common.hpp"                 // glm::min

#include <climits>
#include <unordered_set>
#include <vector>

namespace sfs
{
namespace
{
constexpr int kCoarse = 16; // voxels per water cell (one block); see header
constexpr int kChunk = kTinyChunkSize; // 32
constexpr int kCellsPerChunk =
    kChunk / kCoarse;            // 2 coarse cells per chunk axis
constexpr int kCoarseFull = 256; // water units in a full coarse cell
constexpr double kTick = 1.0 / 30.0;
constexpr int kMaxTicks = 4;
constexpr int kStepBudget =
    20000;                      // coarse cells processed per tick (caps cost)
constexpr int kSeedBudget = 48; // chunks seeded per frame
constexpr float kSettleEps = 0.02f;

int floorDiv(int a, int b)
{
  int q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    --q;
  return q;
}
// Voxel coord -> coarse cell coord, and a coarse cell's centre voxel.
int coarseOf(int v) { return floorDiv(v, kCoarse); }
int centreVoxel(int c) { return c * kCoarse + kCoarse / 2; }
} // namespace

glm::ivec2 WaterSurfaceSystem::tileOf(int x, int z)
{
  return {floorDiv(x, kChunk), floorDiv(z, kChunk)};
}

int WaterSurfaceSystem::waterAt(const glm::ivec3& c) const
{
  const auto it = m_water.find(c);
  return it == m_water.end() ? 0 : it->second;
}

int WaterSurfaceSystem::capacityAt(const glm::ivec3& c) const
{
  // The coarse cell is water-able iff its centre voxel is loaded, open terrain.
  const int wx = centreVoxel(c.x), wy = centreVoxel(c.y), wz = centreVoxel(c.z);
  if (!m_world->hasColumn(wx, wz) || m_world->solidAt(wx, wy, wz))
    return 0;
  return kCoarseFull;
}

float WaterSurfaceSystem::surfaceLevel(const glm::ivec3& c) const
{
  return static_cast<float>(c.y) +
         static_cast<float>(waterAt(c)) / static_cast<float>(kCoarseFull);
}

void WaterSurfaceSystem::setWaterAt(const glm::ivec3& c, int amount)
{
  if (amount <= 0)
    m_water.erase(c);
  else
    m_water[c] = static_cast<std::uint16_t>(glm::min(amount, kCoarseFull));
  m_dirtyTiles.insert(tileOf(centreVoxel(c.x), centreVoxel(c.z)));
}

int WaterSurfaceSystem::topWaterCoarse(int cx, int cz) const
{
  const int wx = centreVoxel(cx), wz = centreVoxel(cz);
  if (!m_world->hasColumn(wx, wz))
    return INT_MIN;
  const int bed = m_world->surfaceTop(wx, wz); // voxel
  if (bed >= m_sea)
    return INT_MIN; // dry land
  const int cTop = floorDiv(m_sea, kCoarse);
  const int cBed = floorDiv(bed, kCoarse);
  for (int cy = cTop; cy >= cBed; --cy)
    if (waterAt({cx, cy, cz}) > 0)
      return cy;
  return INT_MIN;
}

bool WaterSurfaceSystem::coarseLevel(int cx, int cz, float& outVoxelY) const
{
  const int cy = topWaterCoarse(cx, cz);
  if (cy == INT_MIN)
    return false;
  outVoxelY = surfaceLevel({cx, cy, cz}) * static_cast<float>(kCoarse);
  return true;
}

float WaterSurfaceSystem::fineLevel(int x, int z, bool& found) const
{
  const int cx = coarseOf(x), cz = coarseOf(z);
  float level = 0.0f;
  if (coarseLevel(cx, cz, level))
  {
    found = true;
    return level;
  }
  // Dry coarse cell: take the highest water level among the 8 neighbours, so a
  // shore cell adjacent to the lake adopts the lake's surface (and a fully dry
  // region -- e.g. a drained hole -- stays dry).
  found = false;
  float best = 0.0f;
  for (int dz = -1; dz <= 1; ++dz)
    for (int dx = -1; dx <= 1; ++dx)
    {
      if (dx == 0 && dz == 0)
        continue;
      float nl = 0.0f;
      if (coarseLevel(cx + dx, cz + dz, nl) && (!found || nl > best))
      {
        found = true;
        best = nl;
      }
    }
  return best;
}

bool WaterSurfaceSystem::hasWater(int x, int z) const
{
  bool found = false;
  const float level = fineLevel(x, z, found);
  // Fine waterline: water only where the level clears this column's fine bed.
  return found && level > static_cast<float>(m_world->surfaceTop(x, z));
}

float WaterSurfaceSystem::surfaceAt(int x, int z) const
{
  bool found = false;
  const float level = fineLevel(x, z, found);
  return found ? level : static_cast<float>(m_sea);
}

void WaterSurfaceSystem::wake(const glm::ivec3& centerVoxel, int radiusVoxel)
{
  const glm::ivec3 cc{coarseOf(centerVoxel.x),
                      coarseOf(centerVoxel.y),
                      coarseOf(centerVoxel.z)};
  const int r = glm::min(radiusVoxel / kCoarse + 1, 24);
  const int r2 = r * r;
  for (int dy = -r; dy <= r; ++dy)
    for (int dz = -r; dz <= r; ++dz)
      for (int dx = -r; dx <= r; ++dx)
        if (dx * dx + dy * dy + dz * dz <= r2)
          m_active.insert({cc.x + dx, cc.y + dy, cc.z + dz});
}

void WaterSurfaceSystem::seedNewChunks(int budget)
{
  int done = 0;
  for (const auto& [coord, chunk] : m_world->chunks())
  {
    (void)chunk;
    if (m_seededChunks.count(coord))
      continue;
    m_seededChunks.insert(coord);
    if (coord.y * kChunk >= m_sea)
      continue; // entirely above sea
    bool any = false;
    for (int j = 0; j < kCellsPerChunk; ++j)
      for (int k = 0; k < kCellsPerChunk; ++k)
        for (int i = 0; i < kCellsPerChunk; ++i)
        {
          const glm::ivec3 cc{coord.x * kCellsPerChunk + i,
                              coord.y * kCellsPerChunk + j,
                              coord.z * kCellsPerChunk + k};
          if (centreVoxel(cc.y) >= m_sea)
            continue;
          if (capacityAt(cc) > 0) // centre is open below sea -> full water
          {
            m_water[cc] = static_cast<std::uint16_t>(kCoarseFull);
            any = true;
          }
        }
    if (any)
      m_dirtyTiles.insert({coord.x, coord.z});
    if (++done >= budget)
      break;
  }
}

void WaterSurfaceSystem::releaseUnloaded()
{
  for (const glm::ivec3& coord : m_world->unloadedChunks())
  {
    m_seededChunks.erase(coord);
    for (int j = 0; j < kCellsPerChunk; ++j)
      for (int k = 0; k < kCellsPerChunk; ++k)
        for (int i = 0; i < kCellsPerChunk; ++i)
          m_water.erase({coord.x * kCellsPerChunk + i,
                         coord.y * kCellsPerChunk + j,
                         coord.z * kCellsPerChunk + k});
  }
}

void WaterSurfaceSystem::step(int budget)
{
  if (m_active.empty())
    return;

  std::vector<glm::ivec3> cells(m_active.begin(), m_active.end());
  m_active.clear();

  std::unordered_set<glm::ivec3, TinyIVec3Hash> next;
  const auto touch = [&](const glm::ivec3& c)
  {
    next.insert(c);
    next.insert({c.x + 1, c.y, c.z});
    next.insert({c.x - 1, c.y, c.z});
    next.insert({c.x, c.y + 1, c.z});
    next.insert({c.x, c.y - 1, c.z});
    next.insert({c.x, c.y, c.z + 1});
    next.insert({c.x, c.y, c.z - 1});
  };

  std::size_t i = 0;
  for (int processed = 0; i < cells.size() && processed < budget;
       ++i, ++processed)
  {
    const glm::ivec3 c = cells[i];
    int w = waterAt(c);
    if (w <= 0)
      continue;

    // Flow down into the room below.
    const glm::ivec3 below{c.x, c.y - 1, c.z};
    const int roomBelow = capacityAt(below) - waterAt(below);
    if (roomBelow > 0)
    {
      const int move = glm::min(w, roomBelow);
      setWaterAt(below, waterAt(below) + move);
      w -= move;
      setWaterAt(c, w);
      touch(c);
      touch(below);
    }
    if (w <= 0)
      continue;

    // Spread to equalise the surface with same-height neighbours.
    const glm::ivec3 horiz[4] = {{c.x + 1, c.y, c.z},
                                 {c.x - 1, c.y, c.z},
                                 {c.x, c.y, c.z + 1},
                                 {c.x, c.y, c.z - 1}};
    for (const glm::ivec3& n : horiz)
    {
      const int cap = capacityAt(n);
      if (cap <= 0)
        continue;
      const float diff = surfaceLevel(c) - surfaceLevel(n);
      if (diff <= kSettleEps)
        continue;
      const int room = cap - waterAt(n);
      if (room <= 0)
        continue;
      int move =
          static_cast<int>(diff * static_cast<float>(kCoarseFull) * 0.5f);
      move = glm::min(glm::min(move, w), room);
      if (move <= 0)
        continue;
      setWaterAt(n, waterAt(n) + move);
      w -= move;
      setWaterAt(c, w);
      touch(c);
      touch(n);
      if (w <= 0)
        break;
    }
  }

  // Cells beyond the budget carry over, so a big disturbance re-levels over a
  // few frames instead of stalling the frame.
  for (; i < cells.size(); ++i)
    next.insert(cells[i]);

  m_active = std::move(next);
}

void WaterSurfaceSystem::update(double deltaTime)
{
  ZoneScopedN("WaterSurfaceSystem::update");
  if (!m_world)
    return;
  releaseUnloaded();
  seedNewChunks(kSeedBudget);

  m_accum += deltaTime;
  int ticks = 0;
  while (m_accum >= kTick && ticks < kMaxTicks)
  {
    m_accum -= kTick;
    ++ticks;
    if (m_active.empty())
    {
      m_accum = 0.0;
      break;
    }
    step(kStepBudget);
  }
}

} // namespace sfs
