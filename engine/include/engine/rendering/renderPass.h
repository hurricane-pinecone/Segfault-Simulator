#pragma once

#include <cstdint>

namespace sfs
{

enum class RenderPass : uint8_t
{
  Background = 0,
  Terrain,
  Decals, // persistent terrain stains (blood), under shadows/water/sprites
  Shadow, // terrain (elevation) shadows
  SpriteShadow, // projected actor/sprite shadows, drawn over terrain shadows
  Surfaces,
  Objects,
  Particles,
  Foreground,
  UI,
};

} // namespace sfs
