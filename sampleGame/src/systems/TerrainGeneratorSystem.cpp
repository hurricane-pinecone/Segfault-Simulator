#include "TerrainGeneratorSystem.h"
#include "engine/components/cameraComponent.h"
#include "engine/components/terrainBoundaryComponent.h"
#include "engine/components/waterTileComponent.h"
#include "engine/systems/isometric/isometricRenderSystem.h"
#include "gameObjects/blocks/block.h"
#include "gameObjects/blocks/grass.h"
#include "gameObjects/blocks/sand.h"
#include "glm/glm/ext/vector_float2.hpp"

#include <cmath>
#include <vector>

TerrainGeneratorSystem::TerrainGeneratorSystem(sfs::Scene& scene)
    : m_scene(scene)
{
  m_noise.setSeed(1337);
  m_noise.setFrequency(0.035f);
  m_noise.setType(sfs::Noise::Type::OpenSimplex);
}

void TerrainGeneratorSystem::update(double deltaTime)
{
  // TODO: This will become inefficient if more than 1 camera is added to game.
  // Dunno why there ever would be, but maybe
  auto cameras =
      registry->view<sfs::CameraComponent, sfs::TransformComponent>();

  if (cameras.empty())
    return;

  const auto cameraEntity = cameras.front();
  const auto& transform = cameraEntity.getComponent<sfs::TransformComponent>();
  const auto& camera = cameraEntity.getComponent<sfs::CameraComponent>();

  update(transform.position + camera.offset);
}

void TerrainGeneratorSystem::update(const glm::vec2& cameraWorldPos)
{
  const glm::ivec2 centerTile{
      static_cast<int>(std::floor(cameraWorldPos.x)),
      static_cast<int>(std::floor(cameraWorldPos.y)),
  };

  if (centerTile == m_lastCenterTile)
    return;

  m_lastCenterTile = centerTile;

  constexpr int viewTilesX = 50;
  constexpr int viewTilesY = 50;
  constexpr int padding = 0;

  const int centerX = static_cast<int>(std::floor(cameraWorldPos.x));
  const int centerY = static_cast<int>(std::floor(cameraWorldPos.y));

  const int minX = centerX - viewTilesX / 2 - padding;
  const int maxX = centerX + viewTilesX / 2 + padding;

  const int minY = centerY - viewTilesY / 2 - padding;
  const int maxY = centerY + viewTilesY / 2 + padding;

  bool dirty = false;

  for (int y = minY; y <= maxY; y++)
  {
    for (int x = minX; x <= maxX; x++)
    {
      TilePos tile{x, y};

      if (m_loadedTiles.contains(tile))
        continue;

      dirty |= loadTile(tile);
    }
  }

  dirty |= unloadFarTiles(minX, maxX, minY, maxY);

  if (registry->hasSystem<sfs::IsometricRenderSystem>() && dirty)
    registry->getSystem<sfs::IsometricRenderSystem>().markTerrainDirty();
}

bool TerrainGeneratorSystem::loadTile(TilePos tile)
{
  const int elevation = getCachedElevation(tile.x, tile.y);
  const bool isWater = elevation <= m_waterLevel;

  Block& block =
      isWater ? static_cast<Block&>(m_scene.createObject<SandBlock>(
                    glm::vec2{tile.x, tile.y}, elevation, Block::Shape::Full))
              : static_cast<Block&>(m_scene.createObject<GrassBlock>(
                    glm::vec2{tile.x, tile.y}, elevation, Block::Shape::Full));

  if (isWater)
  {
    sfs::WaterTileComponent water;
    water.elevation = m_waterLevel;
    water.color = sfs::Color{35, 120, 190, 115};

    block.entity().addComponent<sfs::WaterTileComponent>(water);
  }

  sfs::TerrainBoundaryComponent boundary;

  const int west = getCachedElevation(tile.x - 1, tile.y);
  const int east = getCachedElevation(tile.x + 1, tile.y);
  const int north = getCachedElevation(tile.x, tile.y - 1);
  const int south = getCachedElevation(tile.x, tile.y + 1);

  boundary.westExposed = west < elevation;
  boundary.eastExposed = east < elevation;
  boundary.northExposed = north < elevation;
  boundary.southExposed = south < elevation;

  boundary.westBottomElevation = west;
  boundary.eastBottomElevation = east;
  boundary.northBottomElevation = north;
  boundary.southBottomElevation = south;

  boundary.dirty = true;

  if (boundary.westExposed || boundary.eastExposed || boundary.northExposed ||
      boundary.southExposed)
  {
    block.entity().addComponent<sfs::TerrainBoundaryComponent>(boundary);
  }

  m_loadedTiles[tile] = {&block};

  return true;
}

bool TerrainGeneratorSystem::unloadTile(TilePos tile)
{
  auto it = m_loadedTiles.find(tile);

  if (it == m_loadedTiles.end())
    return false;

  for (sfs::GameObject* object : it->second)
  {
    m_scene.destroyObject(object);
  }

  m_elevationCache.erase(it->first);
  m_loadedTiles.erase(it);

  return true;
}

bool TerrainGeneratorSystem::unloadFarTiles(int minX,
                                            int maxX,
                                            int minY,
                                            int maxY)
{
  std::vector<TilePos> toUnload;

  for (const auto& [tile, entities] : m_loadedTiles)
  {
    const bool outside =
        tile.x < minX || tile.x > maxX || tile.y < minY || tile.y > maxY;

    if (outside)
      toUnload.push_back(tile);
  }

  bool dirty = false;

  for (const TilePos& tile : toUnload)
  {
    dirty |= unloadTile(tile);
  }

  return dirty;
}

int TerrainGeneratorSystem::getElevation(int x, int y) const
{
  float n = m_noise.get(static_cast<float>(x), static_cast<float>(y));

  constexpr int minHeight = -4;
  constexpr int maxHeight = 12;

  float normalized = (n + 1.0f) * 0.5f;
  normalized = normalized * normalized;

  return minHeight + static_cast<int>(normalized * (maxHeight - minHeight));
}

int TerrainGeneratorSystem::getCachedElevation(int x, int y)
{
  const TilePos tile{x, y};
  auto it = m_elevationCache.find(tile);

  if (it != m_elevationCache.end())
    return it->second;

  const int elevation = getElevation(x, y);
  m_elevationCache.emplace(tile, elevation);

  return elevation;
}
