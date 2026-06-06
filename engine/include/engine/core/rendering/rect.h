#pragma once

namespace sfs
{

// An axis-aligned integer rectangle (x, y, width, height) -- e.g. a sprite-sheet
// source region. Field-compatible with SDL_Rect, so the runtime can hand it to
// SDL where one is required.
struct Rect
{
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

} // namespace sfs
