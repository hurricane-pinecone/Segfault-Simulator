
#pragma once

#include "glm/glm/common.hpp"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include "glm/glm/geometric.hpp"

// std::numeric_limits (below): libc++ pulls it in transitively, but
// GCC/libstdc++ needs it explicit.
#include <limits> // IWYU pragma: keep -- for GCC

namespace sfs
{

/**
 * @brief Performs Digital Differential Analyzer (DDA) traversal through a 2D
 * tile grid.
 *
 * @return
 * Result of the visitor callback:
 *
 *   true  -> continue traversal
 *   false -> terminate traversal early
 *
 * Walks tile-by-tile along a ray direction, visiting each crossed grid cell
 * exactly once in traversal order.
 *
 * This is significantly more efficient than dense point sampling because the
 * algorithm advances directly from one grid boundary crossing to the next
 * instead of evaluating many intermediate positions.
 *
 * @tparam Visitor
 * Callable type with signature:
 *
 *   bool visit(const glm::ivec2& tile, float travelledDistance)
 *
 * @param start
 * Starting world-space position of the ray.
 *
 * @param direction
 * Ray direction. Does not need to be normalized.
 *
 * @param maxDistance
 * Maximum traversal distance measured along the ray.
 *
 * @param visit
 * Visitor callback invoked once per traversed tile.
 *
 * -----------------------------------------------------------------------------
 * Mathematical intuition
 * -----------------------------------------------------------------------------
 *
 * Consider a ray beginning at:
 *
 *   P₀ = (x₀, y₀)
 *
 * travelling in direction:
 *
 *   d = (dx, dy)
 *
 * The ray may be represented parametrically as:
 *
 *   P(t) = P₀ + t·d
 *
 * where:
 *
 *   t ≥ 0
 *
 * represents distance travelled along the ray.
 *
 * The world is partitioned into unit grid cells bounded by the lattice lines:
 *
 *   x = n
 *   y = m
 *
 * for integers n and m.
 *
 * The core problem is:
 *
 *   determine the exact sequence of grid cells intersected by the ray.
 *
 * Instead of repeatedly sampling arbitrary points along the ray, DDA computes:
 *
 *   • the distance until the next vertical grid crossing
 *   • the distance until the next horizontal grid crossing
 *
 * and advances to whichever crossing occurs first.
 *
 * -----------------------------------------------------------------------------
 * tMaxX / tMaxY
 * -----------------------------------------------------------------------------
 *
 * tMaxX:
 *
 *   distance along the ray until the next vertical grid boundary.
 *
 * tMaxY:
 *
 *   distance along the ray until the next horizontal grid boundary.
 *
 * The smaller of the two determines which neighboring tile the traversal enters
 * next.
 *
 * -----------------------------------------------------------------------------
 * tDeltaX / tDeltaY
 * -----------------------------------------------------------------------------
 *
 * tDeltaX:
 *
 *   distance required to cross one complete tile width in X.
 *
 * tDeltaY:
 *
 *   distance required to cross one complete tile height in Y.
 *
 * After each boundary crossing:
 *
 *   tMaxX ← tMaxX + tDeltaX
 *
 * or:
 *
 *   tMaxY ← tMaxY + tDeltaY
 *
 * This incrementally predicts the next crossing distance without repeatedly
 * solving new ray-grid intersection equations.
 *
 * -----------------------------------------------------------------------------
 * Properties
 * -----------------------------------------------------------------------------
 *
 * • Visits each intersected tile exactly once
 * • Complexity is proportional to traversal distance,
 *   not enclosed area
 * • Particularly efficient for long thin traversals such as:
 *
 *     - terrain shadows
 *     - ray casting
 *     - line-of-sight
 *     - voxel traversal
 *     - visibility checks
 *
 * -----------------------------------------------------------------------------
 * Notes
 * -----------------------------------------------------------------------------
 *
 * • Traversal excludes the starting tile and begins with the first crossed
 *   neighboring tile.
 *
 * • travelledDistance supplied to the visitor corresponds to the distance along
 *   the ray at which traversal entered the tile.
 */
template <typename Visitor>
void walkGridDDA(glm::vec2 start,
                 glm::vec2 direction,
                 float maxDistance,
                 Visitor&& visit)
{
  if (maxDistance <= 0.001f)
    return;

  if (glm::length(direction) <= 0.001f)
    return;

  const glm::vec2 dir = glm::normalize(direction);

  glm::ivec2 tile{
      static_cast<int>(glm::floor(start.x)),
      static_cast<int>(glm::floor(start.y)),
  };

  if (!visit(tile, 0.0f))
    return;

  const int stepX = dir.x >= 0.0f ? 1 : -1;
  const int stepY = dir.y >= 0.0f ? 1 : -1;

  const float nextBoundaryX = dir.x >= 0.0f ? static_cast<float>(tile.x + 1)
                                            : static_cast<float>(tile.x);

  const float nextBoundaryY = dir.y >= 0.0f ? static_cast<float>(tile.y + 1)
                                            : static_cast<float>(tile.y);

  float tMaxX = glm::abs(dir.x) > 0.0001f
                    ? (nextBoundaryX - start.x) / dir.x
                    : std::numeric_limits<float>::infinity();

  float tMaxY = glm::abs(dir.y) > 0.0001f
                    ? (nextBoundaryY - start.y) / dir.y
                    : std::numeric_limits<float>::infinity();

  const float tDeltaX = glm::abs(dir.x) > 0.0001f
                            ? glm::abs(1.0f / dir.x)
                            : std::numeric_limits<float>::infinity();

  const float tDeltaY = glm::abs(dir.y) > 0.0001f
                            ? glm::abs(1.0f / dir.y)
                            : std::numeric_limits<float>::infinity();

  while (true)
  {
    const float travelled = glm::min(tMaxX, tMaxY);

    if (travelled > maxDistance)
      break;

    if (tMaxX < tMaxY)
    {
      tile.x += stepX;
      tMaxX += tDeltaX;
    }
    else
    {
      tile.y += stepY;
      tMaxY += tDeltaY;
    }

    if (!visit(tile, travelled))
      break;
  }
}

} // namespace sfs
