#pragma once

#include <cstdint>

namespace sfs
{

enum class RenderPass : uint8_t
{
  Background = 0,
  Terrain,
  Shadow,       // terrain (elevation) shadows
  SpriteShadow, // projected actor/sprite shadows, drawn over terrain shadows
  Surfaces,
  Objects,
  Foreground,
  UI,
};

} // namespace sfs
