#pragma once

#include "glm/glm/ext/vector_int2.hpp"
#include <vector>

namespace sfs
{

// One column of water to draw a surface for: the tile, the water surface level
// and the solid floor level beneath it, both in elevation levels and FRACTIONAL
// (a per-cell water amount can rest at any height, e.g. on a half block).
// Depth = surface - floor. Render-style-agnostic, so both the entity-based
// water (WaterTile component) and a voxel world can feed the same water surface
// mesh.
struct WaterColumn
{
  glm::ivec2 tile{0, 0};
  float surfaceLevel = 0.0f;
  float floorLevel = 0.0f;
};

// Supplies the water columns to render this frame. A voxel world implements it
// from its water blocks; the water render module reads it instead of scanning
// the ECS when one is set.
class IWaterSurfaceSource
{
public:
  virtual ~IWaterSurfaceSource() = default;
  virtual void collectWaterColumns(std::vector<WaterColumn>& out) const = 0;
};

} // namespace sfs
