#include "../../testHarness.h"

#include <engine/core/voxel/waterCell.h>
#include <engine/core/voxel/waterSim.h>

#include "glm/glm/common.hpp"
#include <map>
#include <tuple>
#include <vector>

using namespace sfs;

namespace
{
// A walled basin: air in [0,width) x {0} x {z>=0}, solid (no water) outside, so
// water is contained and settles instead of spreading off to infinity.
struct TestGrid : IWaterGrid
{
  std::map<std::tuple<int, int, int>, int> cells;
  int width = 1;

  int water(const glm::ivec3& c) const override
  {
    const auto it = cells.find({c.x, c.y, c.z});
    return it == cells.end() ? 0 : it->second;
  }
  int capacity(const glm::ivec3& c) const override
  {
    const bool inside = c.z >= 0 && c.y == 0 && c.x >= 0 && c.x < width;
    return inside ? kWaterFull : 0;
  }
  float surface(const glm::ivec3& c) const override
  {
    if (capacity(c) <= 0)
      return static_cast<float>(c.z * kLevelsPerBlock);
    const int amount = glm::min(water(c), static_cast<int>(kWaterFull));
    const float unitsPerLevel =
        static_cast<float>(kWaterFull / kLevelsPerBlock);
    return static_cast<float>(c.z * kLevelsPerBlock) + amount / unitsPerLevel;
  }
  void setWater(const glm::ivec3& c, int amount) override
  {
    cells[{c.x, c.y, c.z}] = amount;
  }

  int total() const
  {
    int sum = 0;
    for (const auto& [k, v] : cells)
      sum += v;
    return sum;
  }
};

// id 1 = cube, id 2 = slab (for the surface helper).
struct Reg : IBlockRegistry
{
  const BlockType& type(BlockId id) const override
  {
    static BlockType air{};
    static BlockType cube{
        BlockShape::Cube, nullptr, {}, true, true, SurfaceEffect::Type::None};
    static BlockType slab{
        BlockShape::Slab, nullptr, {}, true, true, SurfaceEffect::Type::None};
    if (id == 1)
      return cube;
    if (id == 2)
      return slab;
    return air;
  }
};

// Run the sim from a starting active set until it settles or the cap is hit.
void settle(TestGrid& grid, std::vector<glm::ivec3> active, int maxSteps)
{
  for (int i = 0; i < maxSteps && !active.empty(); ++i)
  {
    std::vector<glm::ivec3> next;
    stepWater(active, grid, next);
    active = next;
  }
}
} // namespace

int main()
{
  TEST("water surface sits on top of the cell's solid (half block)")
  {
    Reg reg;
    // Air cell, full: surface at the top of the cell (z*L + L).
    CHECK(cellWaterSurface(0, kAirBlock, reg, kWaterFull) == 2.0f);
    // Slab cell (lower level solid), capacity is half; full water tops the
    // cell.
    CHECK(cellWaterCapacity(2, reg) == kWaterFull / 2);
    CHECK(cellWaterSurface(0, 2, reg, kWaterFull / 2) == 2.0f); // slab top + 1
    CHECK(cellWaterSurface(0, 2, reg, kWaterFull / 4) ==
          1.5f);                           // half-filled slab
    CHECK(cellWaterCapacity(1, reg) == 0); // cube holds none
  }

  TEST("the sim conserves total volume")
  {
    TestGrid grid;
    grid.setWater({0, 0, 5}, kWaterFull);
    grid.setWater({2, 0, 4}, kWaterFull / 2);
    grid.setWater({0, 1, 3}, kWaterFull);
    const int before = grid.total();

    settle(grid, {{0, 0, 5}, {2, 0, 4}, {0, 1, 3}}, 500);

    CHECK(grid.total() == before);
  }

  TEST("water falls to the floor of a column")
  {
    TestGrid grid; // width 1 = a single column, so it stacks
    grid.setWater({0, 0, 6}, kWaterFull);
    settle(grid, {{0, 0, 6}}, 200);

    CHECK(grid.water({0, 0, 6}) == 0);          // drained away
    CHECK(grid.water({0, 0, 0}) == kWaterFull); // landed on the floor
  }

  TEST("a disturbed pool settles (active set empties)")
  {
    TestGrid grid;
    grid.setWater({0, 0, 4}, kWaterFull);

    std::vector<glm::ivec3> active{{0, 0, 4}};
    bool settled = false;
    for (int i = 0; i < 300; ++i)
    {
      std::vector<glm::ivec3> next;
      stepWater(active, grid, next);
      active = next;
      if (active.empty())
      {
        settled = true;
        break;
      }
    }
    CHECK(settled);
    CHECK(grid.water({0, 0, 0}) == kWaterFull);
  }

  TEST("water spreads to a flat level across a trough")
  {
    TestGrid grid;
    grid.width = 3;
    grid.setWater({0, 0, 0}, kWaterFull); // a heap in one corner of the trough
    const int before = grid.total();

    settle(grid, {{0, 0, 0}}, 500);

    CHECK(grid.total() == before); // conserved
    // Settled flat: each of the three cells holds ~a third, none far off.
    const int a = grid.water({0, 0, 0});
    const int b = grid.water({1, 0, 0});
    const int c = grid.water({2, 0, 0});
    CHECK(a + b + c == before);
    CHECK(glm::abs(a - b) <= 2);
    CHECK(glm::abs(b - c) <= 2);
  }

  return testing::report("waterSimTests");
}
