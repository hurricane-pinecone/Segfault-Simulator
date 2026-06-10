#include "engine/runtime/voxel/tinyVoxelWorld.h"

#include "engine/core/util/profiling.h"
#include "engine/core/voxel/iTinyVoxelGenerator.h"
#include "glm/glm/common.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace sfs
{
namespace
{
int floorDiv(int a, int b)
{
  int q = a / b;
  int r = a % b;
  if (r != 0 && ((r < 0) != (b < 0)))
    --q;
  return q;
}
} // namespace

TinyVoxelWorld::TinyVoxelWorld(int minChunkY, int maxChunkY, int radiusChunks)
    : m_minChunkY(minChunkY), m_maxChunkY(maxChunkY),
      m_radiusChunks(radiusChunks), m_genQueue(2)
{
}

void TinyVoxelWorld::setGenerator(const ITinyVoxelGenerator* generator)
{
  m_gen = generator;
  if (m_gen)
    m_seaLevel = m_gen->seaLevel();
}

bool TinyVoxelWorld::solidAt(int x, int y, int z) const
{
  const glm::ivec3 cc{floorDiv(x, kTinyChunkSize),
                      floorDiv(y, kTinyChunkSize),
                      floorDiv(z, kTinyChunkSize)};
  const auto it = m_chunks.find(cc);
  if (it == m_chunks.end())
    return false;
  return it->second.solid(x - cc.x * kTinyChunkSize,
                          y - cc.y * kTinyChunkSize,
                          z - cc.z * kTinyChunkSize);
}

std::uint32_t TinyVoxelWorld::voxelAt(int x, int y, int z) const
{
  const glm::ivec3 cc{floorDiv(x, kTinyChunkSize),
                      floorDiv(y, kTinyChunkSize),
                      floorDiv(z, kTinyChunkSize)};
  const auto it = m_chunks.find(cc);
  if (it == m_chunks.end())
    return 0u;
  return it->second.at(x - cc.x * kTinyChunkSize,
                       y - cc.y * kTinyChunkSize,
                       z - cc.z * kTinyChunkSize);
}

int TinyVoxelWorld::surfaceTop(int x, int z) const
{
  const auto it = m_surfaceTop.find({x, z});
  return it == m_surfaceTop.end() ? m_minChunkY * kTinyChunkSize : it->second;
}

int TinyVoxelWorld::surfaceBelow(int x, int yStart, int z) const
{
  const int floor = m_minChunkY * kTinyChunkSize;
  for (int y = yStart; y >= floor; --y)
    if (solidAt(x, y, z))
      return y + 1; // air cell on top of the surface
  return floor;
}

void TinyVoxelWorld::markDirty(const glm::ivec3& cc) { m_dirty.insert(cc); }

void TinyVoxelWorld::recomputeSurfaceTop(int x, int z)
{
  const int top = (m_maxChunkY + 1) * kTinyChunkSize - 1;
  const int floor = m_minChunkY * kTinyChunkSize;
  for (int y = top; y >= floor; --y)
    if (solidAt(x, y, z))
    {
      m_surfaceTop[{x, z}] = y + 1;
      return;
    }
  m_surfaceTop[{x, z}] = floor; // all air -> surface at the world floor
}

void TinyVoxelWorld::setVoxel(int x, int y, int z, std::uint32_t color)
{
  const glm::ivec3 cc{floorDiv(x, kTinyChunkSize),
                      floorDiv(y, kTinyChunkSize),
                      floorDiv(z, kTinyChunkSize)};
  const int lx = x - cc.x * kTinyChunkSize;
  const int ly = y - cc.y * kTinyChunkSize;
  const int lz = z - cc.z * kTinyChunkSize;
  m_chunks[cc].set(lx, ly, lz, color);

  markDirty(cc);
  if (lx == 0)
    markDirty(cc + glm::ivec3{-1, 0, 0});
  if (lx == kTinyChunkSize - 1)
    markDirty(cc + glm::ivec3{1, 0, 0});
  if (ly == 0)
    markDirty(cc + glm::ivec3{0, -1, 0});
  if (ly == kTinyChunkSize - 1)
    markDirty(cc + glm::ivec3{0, 1, 0});
  if (lz == 0)
    markDirty(cc + glm::ivec3{0, 0, -1});
  if (lz == kTinyChunkSize - 1)
    markDirty(cc + glm::ivec3{0, 0, 1});

  // Incremental surface update (cheap): baking solid above the surface raises
  // it; carving the topmost voxel forces a rescan; anything below leaves it
  // alone.
  const auto it = m_surfaceTop.find({x, z});
  const int top =
      it == m_surfaceTop.end() ? m_minChunkY * kTinyChunkSize : it->second;
  if (color != 0u)
  {
    if (y + 1 > top)
      m_surfaceTop[{x, z}] = y + 1;
  }
  else if (y + 1 == top)
  {
    recomputeSurfaceTop(x, z);
  }
}

TinyVoxelWorld::ColumnData TinyVoxelWorld::generateColumn(int cx, int cz) const
{
  // PURE: reads only the (const) generator + its own locals, so a pool of
  // workers can run this for many columns at once. No m_chunks/m_surfaceTop.
  ZoneScopedN("TinyVoxelWorld::generateColumn");
  ColumnData cd;
  cd.cx = cx;
  cd.cz = cz;

  const int band = m_maxChunkY - m_minChunkY + 1;
  std::vector<TinyVoxelChunk> local(static_cast<std::size_t>(band));
  for (int cy = m_minChunkY; cy <= m_maxChunkY; ++cy)
    m_gen->generate(
        {cx, cy, cz}, local[static_cast<std::size_t>(cy - m_minChunkY)]);

  // Surface scan from the freshly generated local chunks (not the shared
  // store).
  const int floor = m_minChunkY * kTinyChunkSize;
  cd.surface.assign(kTinyChunkSize * kTinyChunkSize, floor);
  for (int lz = 0; lz < kTinyChunkSize; ++lz)
    for (int lx = 0; lx < kTinyChunkSize; ++lx)
    {
      int top = floor;
      for (int cy = m_maxChunkY; cy >= m_minChunkY && top == floor; --cy)
      {
        const TinyVoxelChunk& ch =
            local[static_cast<std::size_t>(cy - m_minChunkY)];
        for (int ly = kTinyChunkSize - 1; ly >= 0; --ly)
          if (ch.solid(lx, ly, lz))
          {
            top = cy * kTinyChunkSize + ly + 1;
            break;
          }
      }
      cd.surface[static_cast<std::size_t>(lz * kTinyChunkSize + lx)] = top;
    }

  // Keep only the non-empty chunks (move them out of the locals).
  for (int cy = m_minChunkY; cy <= m_maxChunkY; ++cy)
  {
    TinyVoxelChunk& ch = local[static_cast<std::size_t>(cy - m_minChunkY)];
    if (!ch.empty())
      cd.chunks.emplace_back(glm::ivec3{cx, cy, cz}, std::move(ch));
  }
  return cd;
}

void TinyVoxelWorld::mergeColumn(ColumnData&& cd)
{
  // SERIAL: touches the shared store, so it runs on one thread after the gen.
  for (auto& [coord, chunk] : cd.chunks)
  {
    m_chunks.emplace(coord, std::move(chunk));
    // Mark the new chunk AND its neighbours: with async meshing a neighbour may
    // already be meshed (against air), so it must re-mesh to hide the seam
    // faces now occluded by this chunk.
    markDirty(coord);
    markDirty(coord + glm::ivec3{1, 0, 0});
    markDirty(coord + glm::ivec3{-1, 0, 0});
    markDirty(coord + glm::ivec3{0, 1, 0});
    markDirty(coord + glm::ivec3{0, -1, 0});
    markDirty(coord + glm::ivec3{0, 0, 1});
    markDirty(coord + glm::ivec3{0, 0, -1});
  }
  m_loadedColumns.insert({cd.cx, cd.cz});
  for (int lz = 0; lz < kTinyChunkSize; ++lz)
    for (int lx = 0; lx < kTinyChunkSize; ++lx)
      m_surfaceTop[{cd.cx * kTinyChunkSize + lx, cd.cz * kTinyChunkSize + lz}] =
          cd.surface[static_cast<std::size_t>(lz * kTinyChunkSize + lx)];
}

void TinyVoxelWorld::unloadColumn(int cx, int cz)
{
  for (int cy = m_minChunkY; cy <= m_maxChunkY; ++cy)
  {
    const glm::ivec3 coord{cx, cy, cz};
    if (m_chunks.erase(coord) > 0)
      m_unloaded.insert(coord); // renderer frees the GPU mesh
    m_dirty.erase(coord);
  }
  m_loadedColumns.erase({cx, cz});
  for (int lz = 0; lz < kTinyChunkSize; ++lz)
    for (int lx = 0; lx < kTinyChunkSize; ++lx)
      m_surfaceTop.erase({cx * kTinyChunkSize + lx, cz * kTinyChunkSize + lz});
}

void TinyVoxelWorld::update(double /*deltaTime*/)
{
  ZoneScopedN("TinyVoxelWorld::stream");
  if (!m_gen)
    return;

  // Merge at most this many finished columns per frame (each writes 1024
  // surface entries + dirties chunks). Generation itself runs on background
  // threads. Higher = the world (and its water) fills in faster on first load.
  constexpr std::size_t kMergePerFrame = 32;

  const glm::ivec2 fc{
      floorDiv(static_cast<int>(glm::floor(m_focus.x)), kTinyChunkSize),
      floorDiv(static_cast<int>(glm::floor(m_focus.z)), kTinyChunkSize)};

  // On a crossing (or first run): submit gen jobs for new columns + unload far.
  if (fc != m_lastFocusChunk || (m_loadedColumns.empty() && m_inFlight.empty()))
  {
    m_lastFocusChunk = fc;
    const int r = m_radiusChunks;
    const int r2 = r * r;

    std::vector<glm::ivec2> toGen;
    for (int dz = -r; dz <= r; ++dz)
      for (int dx = -r; dx <= r; ++dx)
      {
        if (dx * dx + dz * dz > r2)
          continue;
        const glm::ivec2 col{fc.x + dx, fc.y + dz};
        if (m_loadedColumns.find(col) == m_loadedColumns.end() &&
            m_inFlight.find(col) == m_inFlight.end())
          toGen.push_back(col);
      }
    // Submit nearest first, so workers fill the area around the player first.
    const auto dist2 = [&](const glm::ivec2& c)
    { return (c.x - fc.x) * (c.x - fc.x) + (c.y - fc.y) * (c.y - fc.y); };
    std::sort(toGen.begin(),
              toGen.end(),
              [&](const glm::ivec2& a, const glm::ivec2& b)
              { return dist2(a) < dist2(b); });
    for (const glm::ivec2& col : toGen)
    {
      m_inFlight.insert(col);
      m_genQueue.submit(
          [this, col]
          {
            ColumnData cd = generateColumn(col.x, col.y);
            std::lock_guard<std::mutex> lock(m_genResultsMutex);
            m_genResults.push_back(std::move(cd));
          });
    }

    const int unloadR2 = (r + 2) * (r + 2);
    std::vector<glm::ivec2> toUnload;
    for (const glm::ivec2& col : m_loadedColumns)
    {
      const int dx = col.x - fc.x;
      const int dz = col.y - fc.y;
      if (dx * dx + dz * dz > unloadR2)
        toUnload.push_back(col);
    }
    for (const glm::ivec2& col : toUnload)
      unloadColumn(col.x, col.y);
  }

  // Drain finished columns from the workers and merge a budget of them.
  {
    std::vector<ColumnData> ready;
    {
      std::lock_guard<std::mutex> lock(m_genResultsMutex);
      const std::size_t take = std::min(kMergePerFrame, m_genResults.size());
      ready.insert(ready.end(),
                   std::make_move_iterator(m_genResults.end() - take),
                   std::make_move_iterator(m_genResults.end()));
      m_genResults.resize(m_genResults.size() - take);
    }
    for (ColumnData& cd : ready)
    {
      m_inFlight.erase({cd.cx, cd.cz});
      mergeColumn(std::move(cd));
    }
  }
}

} // namespace sfs
