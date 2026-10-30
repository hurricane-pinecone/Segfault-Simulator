#pragma once

#include "engine/runtime/rendering/space2D.h"

#include <string>

#ifdef ENGINE_WEB
inline const std::string ASSET_ROOT = "/assets/";
#else
inline const std::string ASSET_ROOT = "./assets/";
#endif

inline constexpr int WINDOW_WIDTH = 1600;
inline constexpr int WINDOW_HEIGHT = 1200;
inline constexpr float WORLD_SCALE = 2.0f;

// The isometric grid: 32 x 16 pixel tile diamonds, 8 pixels a height level,
// drawn twice the size of the art (WORLD_SCALE). A world unit is TILE_PIXELS
// sprite pixels.
inline constexpr float TILE_PIXELS = 32.0f;
// How much of the world the camera shows top to bottom, in world units.
inline constexpr float VIEW_HEIGHT =
    static_cast<float>(WINDOW_HEIGHT) / (WORLD_SCALE * TILE_PIXELS);

inline sfs::Space2D isometricSpace()
{
  sfs::Space2D space;
  space.projection = sfs::Space2D::Projection::Isometric;
  space.tilePixels = {TILE_PIXELS, TILE_PIXELS * 0.5f};
  space.heightPixels = 8.0f;
  space.pixelsPerUnit = TILE_PIXELS;
  return space;
}
