#pragma once

#include <cstdint>

namespace sfs
{

enum class RenderPass : uint8_t
{
  Background = 0,
  Terrain,
  Shadow,
  Objects,
  Foreground,
  UI,
};

} // namespace sfs
