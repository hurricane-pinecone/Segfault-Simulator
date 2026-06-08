#pragma once

#include <limits>
namespace sfs
{

constexpr static int EmptyElevation = std::numeric_limits<int>::min();

/**
 * The entity's height level on the isometric grid, which sets how high it sits
 * and how it sorts against terrain. Defaults to EmptyElevation (unassigned)
 * until a terrain/movement system places it.
 *
 * @param int level - grid height level, default EmptyElevation (unassigned)
 */
struct ElevationComponent
{
  int level = EmptyElevation;
  // Continuous elevation in levels (cave-aware), driven by MovementSystem so an
  // actor can fall into and stand inside caves. `level` is its rounded form,
  // the discrete value render/sort read. EmptyElevation `level` = not yet
  // placed.
  float height = 0.0f;

  ElevationComponent(int level = EmptyElevation) : level(level){};
};

} // namespace sfs
