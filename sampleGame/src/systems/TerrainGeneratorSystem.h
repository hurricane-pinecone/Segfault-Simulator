#pragma once

#include "engine/ecs/system.h"
#include "engine/noise/noise.h"
#include "engine/rendering/iTerrainHeightSource.h"
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

class TerrainGeneratorSystem : public sfs::System,
                               public sfs::ITerrainHeightSource
{
public:
  explicit TerrainGeneratorSystem(sfs::Scene& scene);
  void update(double deltaTime) override;

  // Terrain elevation is a pure function of the tile coordinate, so it is known
  // for every tile the moment it is asked for -- no entity needs to exist yet.
  // This feeds the point-light occlusion heightmap a hole-free window.
  int terrainHeightAt(int tileX, int tileY) const override
  {
    return getElevation(tileX, tileY);
  }

private:
  void update(const glm::vec2& cameraWorldPos);
  bool loadTile(TilePos tile);
  bool unloadTile(TilePos tile);
  bool unloadFarTiles(int minX, int maxX, int minY, int maxY);

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
