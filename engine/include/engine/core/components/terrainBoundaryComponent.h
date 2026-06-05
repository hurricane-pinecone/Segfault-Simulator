#pragma once

namespace sfs
{

struct TerrainBoundaryComponent
{
  bool westExposed = false;
  bool eastExposed = false;
  bool northExposed = false;
  bool southExposed = false;

  int westBottomElevation = 0;
  int eastBottomElevation = 0;
  int northBottomElevation = 0;
  int southBottomElevation = 0;

  bool dirty = true;
};

} // namespace sfs
