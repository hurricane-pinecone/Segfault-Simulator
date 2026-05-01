#pragma once

#include <cstdint>
#include <vector>

struct MapData
{
  int width = 0;
  int height = 0;
  std::vector<uint32_t> tiles;
};

class MapLoader
{
public:
  static MapData parseMapFile(const std::string& path);
};
