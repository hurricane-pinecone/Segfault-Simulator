#pragma once

#include "engine/ecs/system.h"
#include "engine/noise/noise.h"
#include "engine/sceneManager/scene.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <unordered_map>

struct TilePos
{
  int x = 0;
  int y = 0;

  bool operator==(const TilePos& other) const
  {
    return x == other.x && y == other.y;
  }
};

struct TilePosHash
{
  std::size_t operator()(const TilePos& p) const
  {
    std::size_t hx = std::hash<int>{}(p.x);
    std::size_t hy = std::hash<int>{}(p.y);
    return hx ^ (hy << 1);
  }
};

class TerrainGeneratorSystem : public sfs::System
{
public:
  explicit TerrainGeneratorSystem(sfs::Scene& scene);
  void update(double deltaTime) override;

private:
  void update(const glm::vec2& cameraWorldPos);
  void loadTile(TilePos tile);
  void unloadTile(TilePos tile);
  void unloadFarTiles(int minX, int maxX, int minY, int maxY);

  int getElevation(int x, int y) const;
  int getCachedElevation(int x, int y);

private:
  sfs::Scene& m_scene;
  sfs::Noise m_noise;

  int m_waterLevel = 0;

  // Used to break out of update if camera hasn't moved
  glm::ivec2 m_lastCenterTile{std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::min()};

  std::unordered_map<TilePos, std::vector<sfs::GameObject*>, TilePosHash>
      m_loadedTiles;
  std::unordered_map<TilePos, int, TilePosHash> m_elevationCache;
};
