#pragma once

#include <limits>
namespace sfs
{

constexpr static int EmptyElevation = std::numeric_limits<int>::min();

struct ElevationComponent
{
  int level = EmptyElevation;

  ElevationComponent(int level = EmptyElevation) : level(level){};
};

} // namespace sfs
