#pragma once

#include "glm/glm/ext/vector_int3.hpp"
#include <vector>

namespace sfs
{

// Abstracts water storage + per-cell capacity/surface, so the step is a pure
// function over provided data (in-memory in tests, chunk-backed in VoxelWorld).
class IWaterGrid
{
public:
  virtual ~IWaterGrid() = default;
  virtual int water(const glm::ivec3& cell) const = 0;
  virtual int capacity(const glm::ivec3& cell) const = 0; // 0 = solid, no water
  virtual float surface(const glm::ivec3& cell) const = 0; // height in levels
  virtual void setWater(const glm::ivec3& cell, int amount) = 0;
};

// One volume-conserving fluid tick over the active cells: water flows DOWN,
// then SPREADS to equalize the surface with its same-z neighbours. Cells are
// visited top-to-bottom so a falling column drains in one pass; water that
// can't move stays put, so a basin fills bottom-up. Every move is an integer
// transfer bounded by the target's free capacity, so total volume is exactly
// conserved and no cell overfills. Cells that moved (+ their neighbours)
// populate `nextActive`; a settled region produces none.
//
// (Pressurised rise through a submerged barrier -- a U-tube equalising via its
// floor -- is NOT modelled; down + spread covers
// lakes/seas/drainage/cave-fill.)
void stepWater(const std::vector<glm::ivec3>& active,
               IWaterGrid& grid,
               std::vector<glm::ivec3>& nextActive);

} // namespace sfs
