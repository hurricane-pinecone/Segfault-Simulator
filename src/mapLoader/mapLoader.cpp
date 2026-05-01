#include "mapLoader.h"
#include "logger/logger.h"
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

MapData MapLoader::parseMapFile(const std::string& path)
{
  std::ifstream mapfile(path);
  MapData map;

  if (!mapfile.is_open())
  {
    LOG_ERROR("Could not open map file: " + path);
    return map;
  }

  std::string line;
  int width = 0;
  int height = 0;

  while (std::getline(mapfile, line))
  {
    std::stringstream ss(line);
    std::string value;

    int currentWidth = 0;

    while (std::getline(ss, value, ','))
    {
      if (value.empty())
        continue;

      // Trim whitespace
      value.erase(remove_if(value.begin(), value.end(), isspace), value.end());

      uint32_t spriteId = static_cast<uint32_t>(std::stoul(value));

      map.tiles.push_back(spriteId);

      currentWidth++;
    }

    if (width == 0)
      width = currentWidth;

    height++;
  }

  map.width = width;
  map.height = height;

  return map;
}
