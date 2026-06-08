#include "engine/core/voxel/waterSim.h"

#include "engine/core/voxel/waterChunk.h"
#include "glm/glm/common.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace sfs
{

namespace
{
struct IVec3Hash
{
  std::size_t operator()(const glm::ivec3& v) const noexcept
  {
    const auto x = static_cast<std::uint32_t>(v.x);
    const auto y = static_cast<std::uint32_t>(v.y);
    const auto z = static_cast<std::uint32_t>(v.z);
    return static_cast<std::size_t>(x * 73856093u ^ y * 19349663u ^
                                    z * 83492791u);
  }
};

// A surface difference below this (in levels) doesn't move water -- lets a near
// flat pool settle instead of churning one unit forever.
constexpr float kSettleEps = 0.02f;
} // namespace

void stepWater(const std::vector<glm::ivec3>& active,
               IWaterGrid& grid,
               std::vector<glm::ivec3>& nextActive)
{
  // 1 elevation level of water = this many units (constant across cells: a
  // cell's capacity scales with its empty height, so the rate doesn't).
  const int unitsPerLevel = kWaterFull / kLevelsPerBlock;

  std::unordered_set<glm::ivec3, IVec3Hash> touched;
  const auto touch = [&](const glm::ivec3& c)
  {
    touched.insert(c);
    touched.insert({c.x + 1, c.y, c.z});
    touched.insert({c.x - 1, c.y, c.z});
    touched.insert({c.x, c.y + 1, c.z});
    touched.insert({c.x, c.y - 1, c.z});
    touched.insert({c.x, c.y, c.z + 1});
    touched.insert({c.x, c.y, c.z - 1});
  };

  // Visit top-to-bottom + de-duplicated, so a falling column drains in one
  // pass.
  std::vector<glm::ivec3> cells;
  {
    std::unordered_set<glm::ivec3, IVec3Hash> seen;
    cells.reserve(active.size());
    for (const glm::ivec3& c : active)
      if (seen.insert(c).second)
        cells.push_back(c);
    std::sort(cells.begin(),
              cells.end(),
              [](const glm::ivec3& a, const glm::ivec3& b)
              { return a.z > b.z; });
  }

  for (const glm::ivec3& c : cells)
  {
    int w = grid.water(c);
    if (w <= 0)
      continue;

    // Flow down.
    const glm::ivec3 below{c.x, c.y, c.z - 1};
    const int roomBelow = grid.capacity(below) - grid.water(below);
    if (roomBelow > 0)
    {
      const int move = glm::min(w, roomBelow);
      grid.setWater(below, grid.water(below) + move);
      w -= move;
      grid.setWater(c, w);
      touch(c);
      touch(below);
    }
    if (w <= 0)
      continue;

    // Spread to equalise the surface with same-z neighbours.
    const glm::ivec3 horiz[4] = {{c.x + 1, c.y, c.z},
                                 {c.x - 1, c.y, c.z},
                                 {c.x, c.y + 1, c.z},
                                 {c.x, c.y - 1, c.z}};
    for (const glm::ivec3& n : horiz)
    {
      if (grid.capacity(n) <= 0)
        continue;
      const float diff = grid.surface(c) - grid.surface(n);
      if (diff <= kSettleEps)
        continue;
      const int room = grid.capacity(n) - grid.water(n);
      if (room <= 0)
        continue;

      int move =
          static_cast<int>(diff * static_cast<float>(unitsPerLevel) * 0.5f);
      move = glm::min(glm::min(move, w), room);
      if (move <= 0)
        continue;

      grid.setWater(n, grid.water(n) + move);
      w -= move;
      grid.setWater(c, w);
      touch(c);
      touch(n);
      if (w <= 0)
        break;
    }
  }

  nextActive.assign(touched.begin(), touched.end());
}

} // namespace sfs
