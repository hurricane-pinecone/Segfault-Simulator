#include "engine/runtime/rendering/grassTiles.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"
#include "glm/glm/matrix.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sfs
{

namespace
{
// Over how much of its rank a blade grows in (meshGrassGrow.wgsl's
// kGrowIn): those ranked up to its kept share x (1 + this) are drawn.
constexpr float kGrowIn = 0.2f;
// The most a tile's level goes up (its lattice's top rank, halved this many
// times over two): the coarsest lattice's blades rank all the way down.
constexpr int kMostLevel = GrassTileCache::kTileLevels - 1;
// A tile's built a step more generous than it needs, so it isn't built again
// as soon as the camera nears; and with fewer only once it needs this many
// steps fewer.
constexpr float kBuildStep = 1.4142f;
constexpr int kShrinkLevels = 3;
// Tiles a side of the groups first found out of view, or too far away.
constexpr int kGroupTiles = 4;

int floorDiv(int value, int by)
{
  return value >= 0 ? value / by : -((-value + by - 1) / by);
}
} // namespace

glm::vec2 grassLatticeRanks(int lattice)
{
  if (lattice >= 3)
    return {0.0f, 1.0f / 64.0f};
  const float top = 1.0f / static_cast<float>(1 << (2 * lattice));
  return {top * 0.25f, top};
}

float grassTileThreshold(int lattice, int level)
{
  return grassLatticeRanks(lattice).y *
         std::exp2(-0.5f * static_cast<float>(level));
}

int grassTileLevel(int lattice, float needed)
{
  const glm::vec2 ranks = grassLatticeRanks(lattice);
  if (needed >= ranks.y)
    return 0;
  // (Above its lowest blade's rank: at most the last level that keeps any.)
  const int most = lattice < 3 ? 3 : kMostLevel;
  if (!(needed > 0.0f))
    return most;
  const int level =
      static_cast<int>(std::floor(-2.0f * std::log2(needed / ranks.y)));
  return std::clamp(level, 0, most);
}

uint32_t grassTileRoom(int lattice, float threshold)
{
  const glm::vec2 ranks = grassLatticeRanks(lattice);
  // Its cells with a blade (the finer lattices' leave a quarter to the next).
  const double cells =
      double(kGrassTileCells) * kGrassTileCells * (lattice < 3 ? 0.75 : 1.0);
  const double share = std::clamp(
      double(threshold - ranks.x) / double(ranks.y - ranks.x), 0.0, 1.0);
  // Ranks are spread evenly: about that share of them, and five standard
  // deviations more.
  const double room =
      std::min(cells,
               std::ceil(cells * share +
                         5.0 * std::sqrt(cells * share * (1.0 - share)) + 8.0));
  return static_cast<uint32_t>((room + kGrassChunk - 1) / kGrassChunk) *
         kGrassChunk;
}

// --- GrassChunkPool ----------------------------------------------------------

std::optional<uint32_t> GrassChunkPool::allocate(uint32_t chunks)
{
  if (chunks == 0)
    return std::nullopt;
  for (auto it = m_free.begin(); it != m_free.end(); ++it)
  {
    if (it->second < chunks)
      continue;
    const uint32_t first = it->first;
    const uint32_t left = it->second - chunks;
    m_free.erase(it);
    if (left > 0)
      m_free.emplace(first + chunks, left);
    m_used += chunks;
    return first;
  }
  return std::nullopt;
}

void GrassChunkPool::free(uint32_t first, uint32_t chunks)
{
  m_used -= std::min(chunks, m_used);
  release(first, chunks);
}

void GrassChunkPool::release(uint32_t first, uint32_t chunks)
{
  if (chunks == 0)
    return;
  auto it = m_free.emplace(first, chunks).first;
  // Joined to the room either side of it.
  if (const auto next = std::next(it);
      next != m_free.end() && it->first + it->second == next->first)
  {
    it->second += next->second;
    m_free.erase(next);
  }
  if (it != m_free.begin())
    if (const auto before = std::prev(it);
        before->first + before->second == it->first)
    {
      before->second += it->second;
      m_free.erase(it);
    }
}

void GrassChunkPool::grow(uint32_t chunks)
{
  if (chunks <= m_size)
    return;
  const uint32_t from = m_size;
  m_size = chunks;
  release(from, chunks - from);
}

// --- GrassTileCache ----------------------------------------------------------

int GrassTileCache::Grid::indexOf(glm::ivec2 tile) const
{
  const glm::ivec2 at = tile - from;
  return at.x >= 0 && at.y >= 0 && at.x < size.x && at.y < size.y
             ? at.y * size.x + at.x
             : -1;
}

void GrassTileCache::setGround(const GrassHeights& heights)
{
  m_heights = heights;
  m_lowest = heights.range(glm::ivec2(0), heights.grid().size);
  clear();
}

void GrassTileCache::clear()
{
  for (uint32_t slot = 0; slot < m_tiles.size(); ++slot)
    if (m_tiles[slot].index >= 0)
      drop(slot);
  m_grids = {};
}

void GrassTileCache::makeGrids(float cell)
{
  const GrassGrid& ground = m_heights.grid();
  if (ground.empty() || !(cell > 0.0f))
    return;
  for (int lattice = 0; lattice < 4; ++lattice)
  {
    Grid& grid = m_grids[lattice];
    grid.side = cell * static_cast<float>(1 << lattice) *
                static_cast<float>(kGrassTileCells);
    grid.from = glm::ivec2(glm::floor(ground.min / grid.side));
    grid.size = glm::ivec2(glm::floor(ground.max / grid.side)) - grid.from + 1;
    const std::size_t count = std::size_t(grid.size.x) * grid.size.y;
    grid.under.assign(count, glm::vec2(std::nanf("")));
    grid.slot.assign(count, -1);
    grid.edited.assign(count, 0);
  }
}

void GrassTileCache::drop(uint32_t slot)
{
  Tile& tile = m_tiles[slot];
  m_pool.free(tile.first, tile.chunks);
  if (tile.index >= 0 && !m_grids[tile.lattice].slot.empty())
    m_grids[tile.lattice].slot[tile.index] = -1;
  if (tile.index >= 0)
    --m_built;
  tile = Tile();
  m_freeSlots.push_back(slot);
}

bool GrassTileCache::built(int lattice, glm::ivec2 origin) const
{
  const Grid& grid = m_grids[lattice];
  const int index =
      grid.indexOf(glm::ivec2(floorDiv(origin.x, kGrassTileCells),
                              floorDiv(origin.y, kGrassTileCells)));
  return index >= 0 && grid.slot[index] >= 0;
}

void GrassTileCache::changed(glm::vec2 min, glm::vec2 max)
{
  if (m_built == 0)
    return;
  for (Grid& grid : m_grids)
  {
    if (grid.slot.empty())
      continue;
    const glm::ivec2 from = glm::max(
        glm::ivec2(glm::floor(min / grid.side)) - grid.from, glm::ivec2(0));
    const glm::ivec2 to = glm::min(
        glm::ivec2(glm::floor(max / grid.side)) - grid.from, grid.size - 1);
    for (int y = from.y; y <= to.y; ++y)
      for (int x = from.x; x <= to.x; ++x)
      {
        const int index = y * grid.size.x + x;
        if (grid.slot[index] < 0)
          continue;
        drop(static_cast<uint32_t>(grid.slot[index]));
        grid.edited[index] = 1;
      }
  }
}

glm::vec2 GrassTileCache::heightsUnder(int lattice, glm::ivec2 tile, int index)
{
  Grid& grid = m_grids[lattice];
  glm::vec2& under = grid.under[index];
  if (!std::isnan(under.x))
    return under;
  // Its texels, and one more round them (a blade's height is between its
  // texel's and those beside it).
  const GrassGrid& ground = m_heights.grid();
  const glm::vec2 low = glm::vec2(tile) * grid.side;
  const glm::vec2 texel = ground.texelSize();
  const glm::ivec2 from(glm::floor((low - ground.min) / texel) - 1.0f);
  const glm::ivec2 to(glm::floor((low + grid.side - ground.min) / texel) +
                      2.0f);
  under = m_heights.range(from, to);
  return under;
}

void GrassTileCache::workOutLevels(const GrassGrowUniforms& grow)
{
  m_levels.thinning = grow.thinning;
  m_levels.far = grow.camera.w;
  // The most a tile's blades need ranked below, at a distance (nearest the
  // camera; a little more, for the GPU's rounding): falling with it.
  const auto needed = [&](double away)
  {
    return double(grassKeptShare(
               grow,
               std::max(static_cast<float>(away) * 0.98f - 0.01f, 0.0f))) *
           (1.0 + kGrowIn) * 1.01;
  };
  // From how far away what's needed is at most `rank` (times `more`).
  const double far = std::max(double(grow.camera.w), 1.0) * 2.0;
  const auto from = [&](double rank, double more)
  {
    if (needed(0.0) * more <= rank)
      return 0.0f;
    double low = 0.0, high = far;
    if (needed(high) * more > rank)
      return std::numeric_limits<float>::max();
    for (int step = 0; step < 48; ++step)
    {
      const double middle = (low + high) * 0.5;
      (needed(middle) * more <= rank ? high : low) = middle;
    }
    return static_cast<float>(high);
  };
  for (int lattice = 0; lattice < 4; ++lattice)
  {
    const int most = lattice < 3 ? 3 : kMostLevel;
    for (int level = 0; level <= kMostLevel; ++level)
    {
      const float threshold = grassTileThreshold(lattice, level);
      m_levels.thresholds[lattice][level] = threshold;
      m_levels.chunks[lattice][level] =
          grassTileRoom(lattice, threshold) / kGrassChunk;
      m_levels.from[lattice][level] = level == 0 ? 0.0f
                                      : level <= most
                                          ? from(threshold, 1.0)
                                          : std::numeric_limits<float>::max();
      m_levels.buildFrom[lattice][level] =
          level == 0      ? 0.0f
          : level <= most ? from(threshold, kBuildStep)
                          : std::numeric_limits<float>::max();
    }
    m_levels.none[lattice] = from(grassLatticeRanks(lattice).x, 1.0);
  }
}

int GrassTileCache::levelAt(const std::array<float, kTileLevels>& from,
                            int lattice,
                            float away)
{
  const int most = lattice < 3 ? 3 : kTileLevels - 1;
  int level = 0;
  while (level < most && away >= from[level + 1])
    ++level;
  return level;
}

std::optional<uint32_t> GrassTileCache::roomFor(uint32_t chunks, uint64_t frame)
{
  if (const auto first = m_pool.allocate(chunks))
    return first;
  const auto more = [&](uint32_t most)
  {
    while (m_pool.size() < most)
    {
      m_pool.grow(std::min(
          std::max(m_pool.size() + m_pool.size() / 2, m_pool.size() + chunks),
          most));
      if (const auto first = m_pool.allocate(chunks))
        return first;
    }
    return std::optional<uint32_t>();
  };
  // More room, up to a point; then let go of tiles not in use this frame,
  // least lately used first; then more room, up to the most.
  if (const auto first =
          more(std::max(m_limits.softChunks, m_limits.startChunks)))
    return first;
  std::vector<uint32_t> unused;
  for (uint32_t slot = 0; slot < m_tiles.size(); ++slot)
    if (m_tiles[slot].index >= 0 && m_tiles[slot].used < frame)
      unused.push_back(slot);
  std::sort(unused.begin(),
            unused.end(),
            [&](uint32_t a, uint32_t b)
            { return m_tiles[a].used < m_tiles[b].used; });
  for (const uint32_t slot : unused)
  {
    drop(slot);
    if (const auto first = m_pool.allocate(chunks))
      return first;
  }
  return more(m_limits.mostChunks);
}

const GrassTileCache::Frame&
GrassTileCache::plan(const GrassGrowUniforms& grow, uint64_t frame, bool debug)
{
  m_frame.builds.clear();
  m_frame.drawn.clear();
  m_frame.chunks = 0;
  m_frame.outOfRoom = 0;
  m_away.clear();
  if (m_pool.size() == 0)
    m_pool.grow(m_limits.startChunks);

  // Built for something else (the field moved, its blades changed, its
  // ground or maps are over another grid): all of them again.
  const Built now{grow.model,
                  glm::vec4(grow.shape.x, grow.shape.y, 0.0f, grow.shape.w),
                  grow.ground,
                  grow.groundSize,
                  grow.maps};
  if (now != m_for)
  {
    clear();
    m_for = now;
  }
  const float cell = grow.shape.w;
  if (m_grids[0].slot.empty())
    makeGrids(cell);
  if (m_grids[0].slot.empty())
    return m_frame;
  // Now and then, those unused a while let go.
  if (frame % 60 == 0)
    for (uint32_t slot = 0; slot < m_tiles.size(); ++slot)
      if (m_tiles[slot].index >= 0 &&
          m_tiles[slot].used + m_limits.unusedFrames < frame)
        drop(slot);

  const glm::mat4& model = grow.model;
  const glm::vec3 camera(grow.camera);
  const glm::vec2 eye(glm::inverse(model) * glm::vec4(camera, 1.0f));
  const float tallest =
      grow.shape.x * std::max(1.0f + grow.shape.y * 0.5f, 0.05f);
  std::array<glm::vec3, 6> planes{};
  std::array<glm::vec3, 6> extents{};
  for (std::size_t p = 0; p < planes.size(); ++p)
  {
    planes[p] = glm::vec3(grow.planes[p]);
    extents[p] = glm::abs(planes[p]);
  }
  if (grow.thinning != m_levels.thinning || grow.camera.w != m_levels.far)
    workOutLevels(grow);
  uint32_t shrinks = 0;

  for (int lattice = 0; lattice < 4; ++lattice)
  {
    const glm::ivec4 span = grow.lattices[lattice];
    Grid& grid = m_grids[lattice];
    if (span.z <= 0 || span.w <= 0)
      continue;
    const glm::ivec2 from =
        glm::max(glm::ivec2(floorDiv(span.x, kGrassTileCells),
                            floorDiv(span.y, kGrassTileCells)),
                 grid.from);
    const glm::ivec2 to =
        glm::min(glm::ivec2(floorDiv(span.x + span.z - 1, kGrassTileCells),
                            floorDiv(span.y + span.w - 1, kGrassTileCells)),
                 grid.from + grid.size - 1);
    const float side = grid.side;
    const float reach = grow.reach[lattice];
    // A box of the ground's own (from `low` to `high`) in the world, by its
    // middle and half its size: how near the camera (none past `reach`), and
    // whether its blades are in view (with room to lean, as the grow pass
    // tests each); unless `inside` all the view's planes, as are its tiles
    // then (and if `whole`, that's noted).
    const auto near = [&](const glm::vec3& middle,
                          const glm::vec3& half,
                          bool inside,
                          bool* whole) -> std::optional<float>
    {
      const glm::vec3 apart =
          glm::max(glm::abs(camera - middle) - half, glm::vec3(0.0f));
      const float apart2 = glm::dot(apart, apart);
      if (apart2 > reach * reach)
        return std::nullopt;
      if (!inside)
      {
        const glm::vec3 bladesMiddle =
            middle + glm::vec3(0, 0, tallest * 0.25f);
        const glm::vec3 bladesHalf =
            half + glm::vec3(0, 0, tallest * 0.25f) + 1.2f * tallest;
        bool all = true;
        for (std::size_t p = 0; p < planes.size(); ++p)
        {
          const float centre =
              glm::dot(planes[p], bladesMiddle) + grow.planes[p].w;
          const float extent = glm::dot(extents[p], bladesHalf);
          if (centre + extent < 0.0f)
            return std::nullopt;
          all = all && centre - extent >= 0.0f;
        }
        if (whole)
          *whole = all;
      }
      return std::sqrt(apart2);
    };
    // Each tile's middle and half size in the world: a step across the
    // ground's own X and Y moves it so far.
    const glm::vec3 acrossX = glm::vec3(model[0]) * side;
    const glm::vec3 acrossY = glm::vec3(model[1]) * side;
    const glm::vec3 up(model[2]);
    const glm::vec3 origin(model[3]);
    const glm::vec3 halfSide = (glm::abs(acrossX) + glm::abs(acrossY)) * 0.5f;
    const auto middleOf = [&](glm::vec2 at, float height)
    { return origin + acrossX * at.x + acrossY * at.y + up * height; };
    // A few tiles a side at a time first, over all the ground's heights.
    for (int groupY = from.y; groupY <= to.y; groupY += kGroupTiles)
      for (int groupX = from.x; groupX <= to.x; groupX += kGroupTiles)
      {
        const glm::ivec2 group(groupX, groupY);
        const glm::ivec2 last = glm::min(group + kGroupTiles - 1, to);
        const glm::vec2 tiles(last - group + 1);
        bool inside = false;
        if (!near(middleOf(glm::vec2(group) + tiles * 0.5f,
                           (m_lowest.x + m_lowest.y) * 0.5f),
                  (glm::abs(acrossX) * tiles.x + glm::abs(acrossY) * tiles.y) *
                          0.5f +
                      glm::abs(up) * ((m_lowest.y - m_lowest.x) * 0.5f),
                  false,
                  &inside))
          continue;
        for (int y = group.y; y <= last.y; ++y)
          for (int x = group.x; x <= last.x; ++x)
          {
            const glm::ivec2 at(x, y);
            const int index =
                (y - grid.from.y) * grid.size.x + (x - grid.from.x);
            const glm::vec2 heights = heightsUnder(lattice, at, index);
            if (heights.x > heights.y)
              continue; // no ground
            const glm::vec3 middle =
                middleOf(glm::vec2(at) + 0.5f, (heights.x + heights.y) * 0.5f);
            const std::optional<float> found =
                near(middle,
                     halfSide + glm::abs(up) * ((heights.y - heights.x) * 0.5f),
                     inside,
                     nullptr);
            if (!found)
              continue;
            const float away = *found;
            // The most it needs (nearest the camera), if any of its blades rank
            // that low.
            if (away >= m_levels.none[lattice])
              continue;
            const int level = levelAt(m_levels.from[lattice], lattice, away);

            int slot = grid.slot[index];
            if (slot >= 0)
            {
              const Tile& tile = m_tiles[slot];
              // Built with too few, or with far more than it needs: again.
              const bool more = tile.level > level;
              const bool fewer = tile.level + kShrinkLevels <= level &&
                                 shrinks < m_limits.shrinksPerFrame;
              if (more || fewer)
              {
                shrinks += fewer ? 1 : 0;
                drop(static_cast<uint32_t>(slot));
                slot = -1;
              }
            }
            if (slot < 0)
            {
              const int buildLevel =
                  levelAt(m_levels.buildFrom[lattice], lattice, away);
              const uint32_t chunks = m_levels.chunks[lattice][buildLevel];
              // A slot for its count (letting go of the least lately used
              // unused tile, if they're all taken).
              if (m_freeSlots.empty() && m_tiles.size() >= m_limits.mostTiles)
              {
                uint32_t oldest = UINT32_MAX;
                for (uint32_t other = 0; other < m_tiles.size(); ++other)
                  if (m_tiles[other].used < frame &&
                      (oldest == UINT32_MAX ||
                       m_tiles[other].used < m_tiles[oldest].used))
                    oldest = other;
                if (oldest != UINT32_MAX)
                  drop(oldest);
              }
              const std::optional<uint32_t> first =
                  m_freeSlots.empty() && m_tiles.size() >= m_limits.mostTiles
                      ? std::nullopt
                      : roomFor(chunks, frame);
              if (!first)
              {
                ++m_frame.outOfRoom;
                continue;
              }
              if (!m_freeSlots.empty())
              {
                slot = static_cast<int>(m_freeSlots.back());
                m_freeSlots.pop_back();
              }
              else
              {
                slot = static_cast<int>(m_tiles.size());
                m_tiles.emplace_back();
              }
              Tile& tile = m_tiles[slot];
              tile.lattice = lattice;
              tile.index = index;
              tile.first = *first;
              tile.chunks = chunks;
              tile.level = buildLevel;
              tile.built = frame;
              tile.edited = grid.edited[index] != 0;
              grid.edited[index] = 0;
              grid.slot[index] = slot;
              ++m_built;
            }

            Tile& tile = m_tiles[slot];
            tile.used = frame;
            GrassTileEntry entry;
            entry.origin = at * kGrassTileCells;
            entry.lattice = static_cast<uint32_t>(lattice);
            if (debug && tile.built + m_limits.freshFrames > frame)
              entry.lattice |=
                  kGrassTileFresh | (tile.edited ? kGrassTileEdited : 0);
            entry.slot = static_cast<uint32_t>(slot);
            entry.first = tile.first * kGrassChunk;
            entry.room = tile.chunks * kGrassChunk;
            entry.threshold = m_levels.thresholds[lattice][tile.level];
            entry.heights = heights;
            if (tile.built == frame)
              m_frame.builds.push_back(entry);
            // (Its blades are kept in rows from its low Y: grown from its high
            // side first, if that's the camera's.)
            if (eye.y > (static_cast<float>(y) + 0.5f) * side)
              entry.lattice |= kGrassTileReversed;
            m_frame.drawn.push_back(entry);
            m_away.push_back(glm::length(camera - middle));
          }
      }
  }

  // Nearest first (by their middles): the grow pass lists their blades about
  // in that order, so they're drawn about front to back (the nearer hiding
  // the farther, which then aren't shaded).
  m_order.resize(m_frame.drawn.size());
  for (uint32_t i = 0; i < m_order.size(); ++i)
    m_order[i] = i;
  std::sort(m_order.begin(),
            m_order.end(),
            [&](uint32_t a, uint32_t b) { return m_away[a] < m_away[b]; });
  m_sorted.clear();
  for (const uint32_t i : m_order)
  {
    GrassTileEntry entry = m_frame.drawn[i];
    entry.firstChunk = m_frame.chunks;
    m_frame.chunks += entry.room / kGrassChunk;
    m_sorted.push_back(entry);
  }
  std::swap(m_frame.drawn, m_sorted);
  return m_frame;
}

} // namespace sfs
